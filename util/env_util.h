// Copyright 2026 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_ENV_UTIL_H_
#define STORAGE_LEVELDB_UTIL_ENV_UTIL_H_

#include <cstdint>
#include <functional>
#include <string>

#include "leveldb/status.h"

namespace leveldb {

class Env;

// Copy src to a new file target, reading until EOF using bounded memory. Sync
// and close the destination before returning success. Does not sync directories
// or atomically publish the destination; use a private staging directory.
//
// REQUIRES: src remains unchanged during the copy. The caller exclusively
// controls target's directory and prevents concurrent creation of target.
// Existing targets are rejected, but Env has no atomic exclusive-create API.
//
// After successfully opening target, a failure closes and removes the partial
// file. The first error is preserved unless removal also fails, in which case
// an IOError describes both errors and target may remain. A destination-open
// failure is returned without removing a path whose ownership is uncertain.
Status CopyFileContents(Env* env, const std::string& src,
                        const std::string& target);

// Copy exactly the first length bytes of src to a new file target. Stops at
// length without reading any later bytes, even if src grows during the copy.
// Returns IOError on EOF before length; it does not wait for more data.
// A zero length creates an empty file, but src must still be openable.
//
// REQUIRES: The caller selects a readable prefix whose bytes remain unchanged
// during copying. Appends beyond the prefix are allowed. For logs, the caller
// must flush the source's write buffer and choose a complete record boundary.
// This helper neither selects the checkpoint state nor parses log records.
// The destination ownership, Sync/Close, and failure cleanup rules of
// CopyFileContents apply. This always copies and never creates a hard link.
Status CopyFilePrefix(Env* env, const std::string& src,
                      const std::string& target, uint64_t length);

// Link an immutable file, falling back to CopyFileContents only for
// NotSupported. Other link errors are returned unchanged. The CopyFileContents
// preconditions apply. A successful link shares the source's contents and does
// not sync the source or either directory. Growing logs must not use this
// helper.
Status LinkOrCopyFile(Env* env, const std::string& src,
                      const std::string& target);

// Publish a prepared checkpoint directory under the same existing parent:
// SyncDir(parent/staging_name), RenameFile to parent/checkpoint_name, then
// SyncDir(parent). Names must be distinct single path components, not "." or
// "..". An existing destination entry (including a dangling symlink) is
// rejected.
//
// REQUIRES: The caller exclusively controls both names throughout the call.
// The existence check and rename are not an atomic no-replace operation.
// The parent must already be durable, and the staging path must be a real
// directory, not a symlink. All file data (including linked source files) and
// any child directories must already be synced; writers must be closed.
// The Env must provide atomic directory rename semantics on this filesystem.
//
// renamed must not be null. It is set to false initially, and to true as soon
// as RenameFile returns OK, even if the final parent SyncDir fails. In that
// case the checkpoint is visible but its durability is uncertain. Do not
// delete the final path in response to this error. This function never removes
// either path; cleanup/recovery after a failure belongs to the caller. A failed
// rename may have an ambiguous outcome on filesystems such as NFS.
Status FinalizeCheckpointDirectory(Env* env, const std::string& parent,
                                   const std::string& staging_name,
                                   const std::string& checkpoint_name,
                                   bool* renamed);

// Create a private staging directory, call populate with its path, then call
// FinalizeCheckpointDirectory. Reject existing staging or destination entries;
// never remove anything if CreateDir did not return OK. renamed has the same
// meaning as in FinalizeCheckpointDirectory and must not be null.
//
// REQUIRES: The path ownership, parent durability, and Env rename requirements
// of FinalizeCheckpointDirectory apply. CreateDir must create a new, empty
// directory or fail if the path exists. populate must return a Status without
// throwing, close all writers before returning, and sync file data on success.
// It may create files only directly in staging, must not rename/replace
// staging, and must not modify the parent or destination. It supplies
// checkpoint data and metadata; this helper does not select DB state or
// construct a MANIFEST.
//
// On failure before a successful rename, attempt to remove all staging files
// and the empty directory, then sync the parent. Cleanup is non-recursive;
// unexpected child directories are left in place and reported as an error.
// RemoveFile must unlink symlinks without following them. A failure to remove
// one file does not prevent attempts to remove the others. On cleanup success,
// preserve the original status; otherwise return IOError describing both the
// original failure and the first cleanup error. No final path is ever removed,
// including after an ambiguous rename error or a failed parent sync.
Status CreateCheckpointDirectory(
    Env* env, const std::string& parent, const std::string& staging_name,
    const std::string& checkpoint_name,
    const std::function<Status(const std::string&)>& populate, bool* renamed);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_ENV_UTIL_H_
