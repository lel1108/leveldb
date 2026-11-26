// Copyright (c) 2026 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/checkpoint.h"

#include <memory>
#include <unordered_set>

#include "db/filename.h"
#include "leveldb/env.h"
#include "util/env_util.h"

namespace leveldb {
namespace {

bool IsPlainFileName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of("/\\") == std::string::npos &&
         name.find('\0') == std::string::npos;
}

bool HasFileType(const std::string& name, FileType expected) {
  uint64_t number;
  FileType type;
  return IsPlainFileName(name) && ParseFileName(name, &number, &type) &&
         type == expected;
}

Status ValidatePlan(const CheckpointPlan& plan) {
  if (!HasFileType(plan.live_log_file, kLogFile) ||
      !HasFileType(plan.manifest_file, kDescriptorFile)) {
    return Status::InvalidArgument("invalid checkpoint log or MANIFEST name");
  }
  std::unordered_set<std::string> names = {plan.live_log_file,
                                           plan.manifest_file};
  for (const std::string& name : plan.link_files) {
    if ((!HasFileType(name, kTableFile) && !HasFileType(name, kLogFile)) ||
        !names.insert(name).second) {
      return Status::InvalidArgument("invalid or duplicate checkpoint file",
                                     name);
    }
  }
  return Status::OK();
}

Status SplitCheckpointPath(const std::string& path, std::string* parent,
                           std::string* name) {
  if (path.empty() || path.find('\0') != std::string::npos) {
    return Status::InvalidArgument("invalid checkpoint path");
  }
  std::string trimmed = path;
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
  const size_t separator = trimmed.find_last_of('/');
  if (separator == std::string::npos) {
    *parent = ".";
    *name = trimmed;
  } else {
    *parent = separator == 0 ? "/" : trimmed.substr(0, separator);
    *name = trimmed.substr(separator + 1);
  }
  if (!IsPlainFileName(*name)) {
    return Status::InvalidArgument("invalid checkpoint directory name");
  }
  return Status::OK();
}

// A link shares the source's data, which may still be dirty for a closed log.
// Open without truncation and sync without appending any bytes.
Status LinkOrCopyAndSync(Env* env, const std::string& source,
                         const std::string& target) {
  Status s = env->LinkFile(source, target);
  if (s.IsNotSupportedError()) return CopyFileContents(env, source, target);
  if (!s.ok()) return s;

  WritableFile* raw_file = nullptr;
  s = env->NewAppendableFile(target, &raw_file);
  if (s.IsNotSupportedError()) {
    // Remove the link first: copying onto it would truncate the source too.
    s = env->RemoveFile(target);
    if (!s.ok()) return s;
    return CopyFileContents(env, source, target);
  }
  if (!s.ok()) return s;
  std::unique_ptr<WritableFile> file(raw_file);
  s = file->Sync();
  const Status close_status = file->Close();
  return s.ok() ? close_status : s;
}

Status WriteCurrent(Env* env, const std::string& directory,
                    const std::string& manifest) {
  WritableFile* raw_file = nullptr;
  Status s = env->NewWritableFile(CurrentFileName(directory), &raw_file);
  if (!s.ok()) return s;
  std::unique_ptr<WritableFile> file(raw_file);
  s = file->Append(manifest + "\n");
  if (s.ok()) s = file->Sync();
  const Status close_status = file->Close();
  return s.ok() ? close_status : s;
}

}  // namespace

Status ExecuteCheckpoint(Env* env, const CheckpointPlan& plan,
                         const std::string& db_dir,
                         const std::string& checkpoint_dir) {
  if (env == nullptr || db_dir.empty() ||
      db_dir.find('\0') != std::string::npos) {
    return Status::InvalidArgument("invalid checkpoint source or Env");
  }
  Status s = ValidatePlan(plan);
  if (!s.ok()) return s;
  std::string parent, name;
  s = SplitCheckpointPath(checkpoint_dir, &parent, &name);
  if (!s.ok()) return s;

  // Require directory durability support before creating anything, including
  // for an Env whose CreateDir does not create a real directory (e.g. MemEnv).
  s = env->SyncDir(parent);
  if (!s.ok()) return s;
  bool renamed = false;
  return CreateCheckpointDirectory(
      env, parent, name + ".tmp", name,
      [&](const std::string& staging) {
        for (const std::string& file : plan.link_files) {
          Status result =
              LinkOrCopyAndSync(env, db_dir + "/" + file, staging + "/" + file);
          if (!result.ok()) return result;
        }
        Status result = CopyFilePrefix(env, db_dir + "/" + plan.live_log_file,
                                       staging + "/" + plan.live_log_file,
                                       plan.live_log_size);
        if (!result.ok()) return result;
        result = CopyFilePrefix(env, db_dir + "/" + plan.manifest_file,
                                staging + "/" + plan.manifest_file,
                                plan.manifest_size);
        if (!result.ok()) return result;
        return WriteCurrent(env, staging, plan.manifest_file);
      },
      &renamed);
}

}  // namespace leveldb
