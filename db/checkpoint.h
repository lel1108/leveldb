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
  // Hard-link each one; if linking fails, copy it whole.
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

// Checkpoint, file-layer part: carry out *plan. Copies the files it lists
// from db_dir into a new directory checkpoint_dir and writes the copy's
// CURRENT file. Hard-links where possible, copies otherwise. Must leave no
// half-built checkpoint_dir behind if it fails (build it under a temporary
// name, sync it, then rename it into place).
// Implemented in db/checkpoint.cc.
Status ExecuteCheckpoint(Env* env, const CheckpointPlan& plan,
                         const std::string& db_dir,
                         const std::string& checkpoint_dir);
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_CHECKPOINT_H_
