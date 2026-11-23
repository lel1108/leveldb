# Online checkpoints (course project)

`DB::CreateCheckpoint(path)` creates a directory that can be opened with
`DB::Open()`. The source DB can keep accepting reads and writes. The checkpoint
contains the committed state selected by the DB layer during the call; later
writes are excluded. This implementation targets the POSIX Env on Linux.

## Use

```cpp
leveldb::Status status = db->CreateCheckpoint("/data/checkpoints/checkpoint-1");
if (status.ok()) {
  leveldb::DB* copy = nullptr;
  status = leveldb::DB::Open(options, "/data/checkpoints/checkpoint-1", &copy);
  // Use copy and delete it when finished.
}
```

The parent must already exist and be durable. Reserve both the destination and
its sibling `<destination>.tmp` for this operation. Neither entry may already
exist, including a dangling symlink. Do not create or replace these paths
concurrently. Paths use `/`; trailing `/` is ignored when choosing the names.
The existence check and rename are not an atomic no-replace operation.

## DB and filesystem boundary

`DBImpl::CreateCheckpoint()` calls `PrepareCheckpoint()`, `ExecuteCheckpoint()`,
and then `FinishCheckpoint()`, even when execution returns an error.

The DB layer selects `CheckpointPlan` in `db/checkpoint.h` while holding its
mutex and defers deletion of source files until execution finishes. Committed
log and MANIFEST lengths exclude records whose writes or version installation
have not completed. The filesystem layer does not choose record boundaries or
reconstruct a MANIFEST.

`ExecuteCheckpoint()` in `db/checkpoint.cc` validates the names and then builds
the sibling temporary directory using the helpers in `util/env_util.cc`:

1. Hard-link immutable SSTables and closed logs. Sync each linked inode through
   an append handle without appending bytes. If linking is `NotSupported`
   (including POSIX `EXDEV`), copy and sync the entire file instead. If opening
   the append handle is unsupported, remove the owned link before copying so
   the copy cannot truncate the source inode. Other I/O errors are propagated.
2. Copy exactly `live_log_size` and `manifest_size` bytes into independent
   files. A short source is an error. Bytes appended after those prefixes are
   ignored. Each destination is synced and closed.
3. Write `manifest_file + "\n"` into `CURRENT`, sync it, and close it.
4. Sync the temporary directory, rename it to the final path under the same
   parent, and sync the parent directory.

The source's selected prefixes must already be flushed and remain readable.
The current DB layer flushes log records before publishing their committed
length and syncs MANIFEST records before publishing the corresponding version.
File deletion remains deferred throughout the filesystem work.

## Failures and limits

Before rename, execution attempts to remove only files in its own temporary
directory and then the empty directory. Existing paths are never cleaned up.
Cleanup is non-recursive; cleanup errors are included in the returned status.
An interrupted process can leave `.tmp` behind; automatic crash recovery of
stale staging directories is not implemented.

After rename, failure to sync the parent returns an error but retains the
complete visible checkpoint. Its persistence across a crash is uncertain.
Never automatically delete a final path solely because the call returned an
error. An ambiguous rename error can also leave a final path on filesystems
such as NFS.

Only one checkpoint per source DB is supported at a time. Directory Sync and
atomic directory rename must be provided by the Env. Windows and memory Env
currently return `NotSupported` for directory Sync. This does not implement
independent Windows checkpoint support, power-loss testing, or automatic
cleanup after process termination.

## Build and test

From the repository root, with the CMake dependencies checked out:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF \
  -DLEVELDB_BUILD_TESTS=ON -DLEVELDB_BUILD_BENCHMARKS=OFF
cmake --build build --parallel
export TEST_TMPDIR="$(mktemp -d /var/tmp/leveldb-checkpoint-tests.XXXXXX)"
./build/leveldb_tests --gtest_filter='DBTest.*Checkpoint*:DBTest.*FileDeletions*'
./build/env_posix_test --gtest_filter='EnvPosixExecuteCheckpointTest.*'
ctest --test-dir build --output-on-failure --timeout 180
```

The API tests in `db/db_test.cc` cover reopening the checkpoint, source
compaction/deletion, empty databases, existing destinations, failed copies and
syncs followed by retry, and atomic batches while writing and compacting.
The POSIX tests check inode sharing, prefix lengths, `CURRENT`, unsupported
operation fallback, original-file preservation, and injected publication
failures. These are functional tests, not throughput or power-loss benchmarks.
