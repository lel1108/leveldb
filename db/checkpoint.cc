// Copyright (c) 2026 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/checkpoint.h"

#include "leveldb/env.h"

namespace leveldb {

// File-layer part of a checkpoint. See db/checkpoint.h for what it must do.
// TODO(file layer): replace this placeholder with the real implementation.
Status ExecuteCheckpoint(Env* env, const CheckpointPlan& plan,
                         const std::string& db_dir,
                         const std::string& checkpoint_dir) {
  return Status::NotSupported("ExecuteCheckpoint is not implemented yet");
}

}  // namespace leveldb
