// Copyright (c) 2026 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_CHECKPOINT_H_
#define STORAGE_LEVELDB_DB_CHECKPOINT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "leveldb/status.h"

namespace leveldb {

// What a checkpoint must copy, decided by the DB layer at one instant
// (DBImpl::PrepareCheckpoint) and carried out by the file layer.
// All names are plain file names inside the DB directory, not full paths.
struct CheckpointPlan {
  // Files that never change once written (table files and closed logs).
  // Hard-link and sync each one; copy it whole when linking is unsupported
  // (including across filesystems). Other I/O errors are returned unchanged.
  std::vector<std::string> link_files;

  // The log file currently being appended to. Copy only its first
  // live_log_size bytes: later bytes were written after the checkpoint.
  std::string live_log_file;
  uint64_t live_log_size = 0;

  // The MANIFEST in use. It is also append-only, so copy only its first
  // manifest_size bytes. The copy's CURRENT file must name this file.
  std::string manifest_file;
  uint64_t manifest_size = 0;
};

class Env;

// Checkpoint, file-layer part: carry out plan. Copies the files it lists
// from db_dir into a new directory checkpoint_dir and writes the copy's
// CURRENT file. Build under checkpoint_dir + ".tmp", sync all file data and
// the staging directory, rename into place, then sync the parent directory.
// Existing checkpoint or staging entries are rejected and preserved. A failure
// before rename attempts to remove staging; cleanup errors are reported too.
// No final path is deleted: if the final parent sync fails, return an error
// but retain the complete, visible checkpoint whose durability is uncertain.
//
// REQUIRES: DB-layer deletion hold stays active for this call. Immutable files
// and the selected, flushed log/MANIFEST prefixes remain readable and
// unchanged. The destination's parent already exists and is durable. The caller
// exclusively controls the final and staging names; checking existence and
// renaming is not an atomic no-replace operation. Env must support directory
// sync and atomic directory rename. Paths use '/' as the separator; trailing
// '/' is ignored. Implemented in db/checkpoint.cc.
Status ExecuteCheckpoint(Env* env, const CheckpointPlan& plan,
                         const std::string& db_dir,
                         const std::string& checkpoint_dir);
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_CHECKPOINT_H_
