// Copyright 2026 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/env_util.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "leveldb/env.h"

namespace leveldb {

namespace {

bool IsCheckpointComponent(const std::string& name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of("/\\") == std::string::npos &&
         name.find('\0') == std::string::npos;
}

bool ValidCheckpointPaths(const std::string& parent,
                          const std::string& staging_name,
                          const std::string& checkpoint_name) {
  return !parent.empty() && parent.find('\0') == std::string::npos &&
         IsCheckpointComponent(staging_name) &&
         IsCheckpointComponent(checkpoint_name) &&
         staging_name != checkpoint_name;
}

std::string CheckpointPath(const std::string& parent, const std::string& name) {
  return (parent.back() == '/' ? parent : parent + "/") + name;
}

// Only called after this operation has successfully created staging. The
// caller keeps this directory and its parent stable until cleanup completes.
Status CleanupCheckpointDirectory(Env* env, const std::string& parent,
                                  const std::string& staging) {
  std::vector<std::string> children;
  Status status = env->GetChildren(staging, &children);
  // A failed rename may have moved the directory on some filesystems. Never
  // look for it under the final name or attempt to delete the final path.
  if (status.IsNotFound()) return Status::OK();
  if (!status.ok()) return status;

  // Validate the whole listing before deleting anything. Custom Envs must not
  // let an absolute path or a nested name escape the private staging directory.
  for (const std::string& child : children) {
    if (child != "." && child != ".." && !IsCheckpointComponent(child)) {
      return Status::InvalidArgument("invalid staging child", child);
    }
  }
  for (const std::string& child : children) {
    if (child == "." || child == "..") continue;
    const Status removed = env->RemoveFile(staging + "/" + child);
    if (status.ok() && !removed.ok() && !removed.IsNotFound()) status = removed;
  }
  const Status removed_dir = env->RemoveDir(staging);
  if (!removed_dir.ok() && !removed_dir.IsNotFound()) {
    if (status.ok()) status = removed_dir;
  } else {
    const Status synced = env->SyncDir(parent);
    if (status.ok()) status = synced;
  }
  return status;
}

// Keep an explicit flag so that both zero and UINT64_MAX are valid lengths.
Status DoCopyFile(Env* env, const std::string& src, const std::string& target,
                  bool limit_length, uint64_t remaining) {
  const std::string operation =
      limit_length ? "CopyFilePrefix" : "CopyFileContents";
  if (src == target) {
    return Status::InvalidArgument(
        operation + " source and target are identical", src);
  }
  if (env->FileExists(target)) {
    return Status::InvalidArgument(operation + " target already exists",
                                   target);
  }

  SequentialFile* input_file = nullptr;
  Status status = env->NewSequentialFile(src, &input_file);
  if (!status.ok()) {
    return status;
  }
  std::unique_ptr<SequentialFile> input(input_file);

  WritableFile* output_file = nullptr;
  status = env->NewWritableFile(target, &output_file);
  if (!status.ok()) {
    return status;
  }
  std::unique_ptr<WritableFile> output(output_file);

  char buffer[8192];
  while (status.ok() && (!limit_length || remaining != 0)) {
    const size_t bytes_to_read =
        limit_length
            ? static_cast<size_t>(std::min<uint64_t>(sizeof(buffer), remaining))
            : sizeof(buffer);
    Slice fragment;
    status = input->Read(bytes_to_read, &fragment, buffer);
    if (!status.ok()) {
      break;
    }
    if (fragment.empty()) {
      if (limit_length) {
        status =
            Status::IOError(src, "unexpected EOF while copying file prefix");
      }
      break;
    }
    status = output->Append(fragment);
    if (limit_length && status.ok()) {
      remaining -= fragment.size();
    }
  }
  if (status.ok()) {
    status = output->Sync();
  }
  const Status close_status = output->Close();
  if (status.ok()) {
    status = close_status;
  }
  output.reset();
  input.reset();

  if (!status.ok()) {
    const Status cleanup_status = env->RemoveFile(target);
    if (!cleanup_status.ok()) {
      return Status::IOError(
          status.ToString(),
          "removing partial file " + target + ": " + cleanup_status.ToString());
    }
  }
  return status;
}

}  // namespace

Status CopyFileContents(Env* env, const std::string& src,
                        const std::string& target) {
  return DoCopyFile(env, src, target, false, 0);
}

Status CopyFilePrefix(Env* env, const std::string& src,
                      const std::string& target, uint64_t length) {
  return DoCopyFile(env, src, target, true, length);
}

Status LinkOrCopyFile(Env* env, const std::string& src,
                      const std::string& target) {
  Status status = env->LinkFile(src, target);
  if (status.IsNotSupportedError()) {
    return CopyFileContents(env, src, target);
  }
  return status;
}

Status FinalizeCheckpointDirectory(Env* env, const std::string& parent,
                                   const std::string& staging_name,
                                   const std::string& checkpoint_name,
                                   bool* renamed) {
  if (renamed == nullptr) {
    return Status::InvalidArgument("FinalizeCheckpointDirectory",
                                   "null renamed");
  }
  *renamed = false;
  if (!ValidCheckpointPaths(parent, staging_name, checkpoint_name)) {
    return Status::InvalidArgument("FinalizeCheckpointDirectory",
                                   "invalid paths");
  }

  // FileExists follows symlinks on POSIX and misses dangling ones. List the
  // parent instead so that no existing destination entry is replaced.
  std::vector<std::string> children;
  Status status = env->GetChildren(parent, &children);
  if (!status.ok()) return status;
  if (std::find(children.begin(), children.end(), checkpoint_name) !=
      children.end()) {
    return Status::InvalidArgument("checkpoint already exists",
                                   checkpoint_name);
  }

  const std::string staging = CheckpointPath(parent, staging_name);
  const std::string checkpoint = CheckpointPath(parent, checkpoint_name);
  status = env->SyncDir(staging);
  if (!status.ok()) return status;
  status = env->RenameFile(staging, checkpoint);
  if (!status.ok()) return status;
  *renamed = true;
  return env->SyncDir(parent);
}

Status CreateCheckpointDirectory(
    Env* env, const std::string& parent, const std::string& staging_name,
    const std::string& checkpoint_name,
    const std::function<Status(const std::string&)>& populate, bool* renamed) {
  if (renamed == nullptr) {
    return Status::InvalidArgument("CreateCheckpointDirectory", "null renamed");
  }
  *renamed = false;
  if (!ValidCheckpointPaths(parent, staging_name, checkpoint_name) ||
      !populate) {
    return Status::InvalidArgument("CreateCheckpointDirectory",
                                   "invalid arguments");
  }
  std::vector<std::string> children;
  Status status = env->GetChildren(parent, &children);
  if (!status.ok()) return status;
  for (const std::string& child : children) {
    if (child == staging_name || child == checkpoint_name) {
      return Status::InvalidArgument("checkpoint path already exists", child);
    }
  }
  const std::string staging = CheckpointPath(parent, staging_name);
  status = env->CreateDir(staging);
  if (!status.ok()) return status;

  status = populate(staging);
  if (status.ok()) {
    status = FinalizeCheckpointDirectory(env, parent, staging_name,
                                         checkpoint_name, renamed);
  }
  if (status.ok() || *renamed) return status;

  const Status cleanup = CleanupCheckpointDirectory(env, parent, staging);
  if (!cleanup.ok()) {
    return Status::IOError(
        status.ToString(),
        "cleaning staging directory " + staging + ": " + cleanup.ToString());
  }
  return status;
}

}  // namespace leveldb
