// Copyright 2026 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/env_util.h"

#include "helpers/memenv/memenv.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "leveldb/env.h"

#include "util/testutil.h"

#include "gtest/gtest.h"

namespace leveldb {
namespace {

// Exercise real in-memory files while injecting failures at each copy stage.
class CopyTestEnv : public EnvWrapper {
 public:
  enum Failure { kNone, kOpen, kCreate, kRead, kAppend, kSync, kClose };

  explicit CopyTestEnv(Env* base) : EnvWrapper(base) {}

  static Status Error(Failure failure) {
    switch (failure) {
      case kOpen:
        return Status::IOError("injected open error");
      case kCreate:
        return Status::IOError("injected create error");
      case kRead:
        return Status::Corruption("injected read error");
      case kAppend:
        return Status::IOError("injected append error");
      case kSync:
        return Status::IOError("injected sync error");
      case kClose:
        return Status::IOError("injected close error");
      default:
        return Status::OK();
    }
  }

  Status NewSequentialFile(const std::string& path,
                           SequentialFile** result) override {
    ++open_calls;
    *result = nullptr;
    if (failure == kOpen) return Error(kOpen);
    SequentialFile* file;
    Status status = target()->NewSequentialFile(path, &file);
    if (status.ok()) {
      *result = new Reader(this, file);
      ++active_readers;
    }
    return status;
  }

  Status NewWritableFile(const std::string& path,
                         WritableFile** result) override {
    ++create_calls;
    *result = nullptr;
    if (failure == kCreate) return Error(kCreate);
    WritableFile* file;
    Status status = target()->NewWritableFile(path, &file);
    if (status.ok()) {
      *result = new Writer(this, file);
      ++active_writers;
    }
    return status;
  }

  Status RemoveFile(const std::string& path) override {
    ++remove_calls;
    removed_with_open_file = active_readers != 0 || active_writers != 0;
    if (fail_remove) return Status::IOError("injected remove error");
    return target()->RemoveFile(path);
  }

  Status LinkFile(const std::string& src, const std::string& dest) override {
    ++link_calls;
    linked_src = src;
    linked_target = dest;
    return link_status;
  }

  Failure failure = kNone;
  bool fail_close = false;
  bool fail_remove = false;
  size_t max_read_size = 8192;
  std::vector<size_t> read_requests;
  std::function<void()> before_read;
  Status link_status = Status::NotSupported("injected unsupported link");
  std::string linked_src;
  std::string linked_target;
  int open_calls = 0;
  int create_calls = 0;
  int read_calls = 0;
  int sync_calls = 0;
  int close_calls = 0;
  int remove_calls = 0;
  int link_calls = 0;
  int active_readers = 0;
  int active_writers = 0;
  bool removed_with_open_file = false;

 private:
  class Reader : public SequentialFile {
   public:
    Reader(CopyTestEnv* env, SequentialFile* file) : env_(env), file_(file) {}
    ~Reader() override { --env_->active_readers; }
    Status Read(size_t n, Slice* result, char* scratch) override {
      env_->read_requests.push_back(n);
      // Fail only after some bytes have already been copied.
      if (++env_->read_calls == 2 && env_->failure == kRead) {
        return Error(kRead);
      }
      if (env_->before_read) env_->before_read();
      return file_->Read(std::min(n, env_->max_read_size), result, scratch);
    }
    Status Skip(uint64_t n) override { return file_->Skip(n); }

   private:
    CopyTestEnv* env_;
    std::unique_ptr<SequentialFile> file_;
  };

  class Writer : public WritableFile {
   public:
    Writer(CopyTestEnv* env, WritableFile* file) : env_(env), file_(file) {}
    ~Writer() override { --env_->active_writers; }
    Status Append(const Slice& data) override {
      Status status = file_->Append(data);
      // A failed write is allowed to leave data in the destination.
      return env_->failure == kAppend ? Error(kAppend) : status;
    }
    Status Close() override {
      ++env_->close_calls;
      Status status = file_->Close();
      return env_->failure == kClose || env_->fail_close ? Error(kClose)
                                                         : status;
    }
    Status Flush() override { return file_->Flush(); }
    Status Sync() override {
      ++env_->sync_calls;
      return env_->failure == kSync ? Error(kSync) : file_->Sync();
    }

   private:
    CopyTestEnv* env_;
    std::unique_ptr<WritableFile> file_;
  };
};

class CopyFileTest : public testing::Test {
 protected:
  CopyFileTest() : base_(NewMemEnv(Env::Default())), env_(base_.get()) {}

  void SetUp() override {
    for (int i = 0; i < 25000; ++i) data_.push_back(static_cast<char>(i % 256));
    ASSERT_LEVELDB_OK(WriteStringToFile(base_.get(), data_, "/source"));
  }

  void ExpectContents(const std::string& path, const std::string& expected) {
    std::string actual;
    ASSERT_LEVELDB_OK(ReadFileToString(base_.get(), path, &actual));
    EXPECT_EQ(expected, actual);
  }

  std::unique_ptr<Env> base_;
  CopyTestEnv env_;
  std::string data_;
};

TEST_F(CopyFileTest, EmptyAndBufferBoundarySizes) {
  for (size_t size : {0, 1, 8191, 8192, 8193, 25000}) {
    SCOPED_TRACE(size);
    const std::string data = data_.substr(0, size);
    ASSERT_LEVELDB_OK(WriteStringToFile(base_.get(), data, "/source"));
    CopyTestEnv env(base_.get());
    ASSERT_LEVELDB_OK(CopyFileContents(&env, "/source", "/target"));
    ExpectContents("/source", data);
    ExpectContents("/target", data);
    EXPECT_EQ(1, env.sync_calls);
    EXPECT_EQ(1, env.close_calls);
    EXPECT_EQ(0, env.active_readers);
    EXPECT_EQ(0, env.active_writers);
    ASSERT_LEVELDB_OK(base_->RemoveFile("/target"));
  }
}

TEST_F(CopyFileTest, ShortReadsContinueUntilEof) {
  env_.max_read_size = 7;
  ASSERT_LEVELDB_OK(CopyFileContents(&env_, "/source", "/target"));
  ExpectContents("/target", data_);
  EXPECT_GT(env_.read_calls, 25000 / 7);
}

TEST_F(CopyFileTest, CopyRemainsIndependentOfSource) {
  ASSERT_LEVELDB_OK(CopyFileContents(&env_, "/source", "/target"));
  ASSERT_LEVELDB_OK(WriteStringToFile(base_.get(), "changed", "/source"));
  ExpectContents("/target", data_);
  ASSERT_LEVELDB_OK(base_->RemoveFile("/source"));
  ExpectContents("/target", data_);
}

TEST_F(CopyFileTest, ExistingTargetAndSamePathArePreserved) {
  ASSERT_LEVELDB_OK(WriteStringToFile(base_.get(), "keep", "/target"));
  EXPECT_TRUE(
      CopyFileContents(&env_, "/source", "/target").IsInvalidArgument());
  EXPECT_TRUE(
      CopyFileContents(&env_, "/source", "/source").IsInvalidArgument());
  ExpectContents("/source", data_);
  ExpectContents("/target", "keep");
  EXPECT_EQ(0, env_.create_calls);
  EXPECT_EQ(0, env_.remove_calls);
}

TEST_F(CopyFileTest, MissingSourceDoesNotCreateTarget) {
  EXPECT_FALSE(CopyFileContents(&env_, "/missing", "/target").ok());
  EXPECT_FALSE(base_->FileExists("/target"));
  EXPECT_EQ(0, env_.create_calls);
  EXPECT_EQ(0, env_.remove_calls);
}

TEST_F(CopyFileTest, FailuresCloseFilesAndRemovePartialTarget) {
  for (CopyTestEnv::Failure failure :
       {CopyTestEnv::kOpen, CopyTestEnv::kCreate, CopyTestEnv::kRead,
        CopyTestEnv::kAppend, CopyTestEnv::kSync, CopyTestEnv::kClose}) {
    SCOPED_TRACE(failure);
    CopyTestEnv env(base_.get());
    env.failure = failure;
    const Status status = CopyFileContents(&env, "/source", "/target");
    EXPECT_EQ(CopyTestEnv::Error(failure).ToString(), status.ToString());
    EXPECT_FALSE(base_->FileExists("/target"));
    ExpectContents("/source", data_);
    const bool opened_target =
        failure != CopyTestEnv::kOpen && failure != CopyTestEnv::kCreate;
    EXPECT_EQ(opened_target ? 1 : 0, env.close_calls);
    EXPECT_EQ(opened_target ? 1 : 0, env.remove_calls);
    EXPECT_EQ(0, env.active_readers);
    EXPECT_EQ(0, env.active_writers);
    EXPECT_FALSE(env.removed_with_open_file);
  }
}

TEST_F(CopyFileTest, FirstErrorSurvivesCloseFailure) {
  env_.failure = CopyTestEnv::kRead;
  env_.fail_close = true;
  const Status status = CopyFileContents(&env_, "/source", "/target");
  EXPECT_EQ(CopyTestEnv::Error(CopyTestEnv::kRead).ToString(),
            status.ToString());
  EXPECT_FALSE(base_->FileExists("/target"));
  EXPECT_EQ(1, env_.close_calls);
}

TEST_F(CopyFileTest, CleanupFailureReportsBothErrors) {
  env_.failure = CopyTestEnv::kRead;
  env_.fail_remove = true;
  const Status status = CopyFileContents(&env_, "/source", "/target");
  EXPECT_TRUE(status.IsIOError());
  EXPECT_NE(std::string::npos, status.ToString().find("injected read error"));
  EXPECT_NE(std::string::npos, status.ToString().find("injected remove error"));
  EXPECT_NE(std::string::npos, status.ToString().find("/target"));
  EXPECT_TRUE(base_->FileExists("/target"));
  EXPECT_EQ(0, env_.active_readers);
  EXPECT_EQ(0, env_.active_writers);
  ExpectContents("/source", data_);
}

TEST_F(CopyFileTest, UnsupportedLinkFallsBackToCopy) {
  ASSERT_LEVELDB_OK(LinkOrCopyFile(&env_, "/source", "/target"));
  EXPECT_EQ(1, env_.link_calls);
  EXPECT_EQ("/source", env_.linked_src);
  EXPECT_EQ("/target", env_.linked_target);
  EXPECT_EQ(1, env_.create_calls);
  ExpectContents("/target", data_);
}

TEST_F(CopyFileTest, MemEnvFallsBackToIndependentCopy) {
  ASSERT_LEVELDB_OK(LinkOrCopyFile(base_.get(), "/source", "/target"));
  ASSERT_LEVELDB_OK(WriteStringToFile(base_.get(), "changed", "/source"));
  ExpectContents("/target", data_);
}

TEST_F(CopyFileTest, OtherLinkResultsDoNotAttemptCopy) {
  for (const Status& result :
       {Status::OK(), Status::NotFound("missing"), Status::IOError("denied"),
        Status::InvalidArgument("bad path")}) {
    SCOPED_TRACE(result.ToString());
    CopyTestEnv env(base_.get());
    env.link_status = result;
    EXPECT_EQ(result.ToString(),
              LinkOrCopyFile(&env, "/source", "/target").ToString());
    EXPECT_EQ(0, env.open_calls);
    EXPECT_EQ(0, env.create_calls);
    EXPECT_EQ(0, env.remove_calls);
  }
}

TEST_F(CopyFileTest, FallbackPreservesExistingTarget) {
  ASSERT_LEVELDB_OK(WriteStringToFile(base_.get(), "keep", "/target"));
  EXPECT_TRUE(LinkOrCopyFile(&env_, "/source", "/target").IsInvalidArgument());
  ExpectContents("/target", "keep");
  EXPECT_EQ(0, env_.create_calls);
  EXPECT_EQ(0, env_.remove_calls);
}

TEST_F(CopyFileTest, FallbackReturnsCopyErrorAndCleansUp) {
  env_.failure = CopyTestEnv::kAppend;
  const Status status = LinkOrCopyFile(&env_, "/source", "/target");
  EXPECT_EQ(CopyTestEnv::Error(CopyTestEnv::kAppend).ToString(),
            status.ToString());
  EXPECT_FALSE(base_->FileExists("/target"));
  ExpectContents("/source", data_);
}

class CopyFilePrefixTest : public CopyFileTest {};

TEST_F(CopyFilePrefixTest, ExactLengthAndBufferBoundaries) {
  for (uint64_t length : {0, 1, 8191, 8192, 8193, 25000}) {
    SCOPED_TRACE(length);
    CopyTestEnv env(base_.get());
    ASSERT_LEVELDB_OK(CopyFilePrefix(&env, "/source", "/target", length));
    ExpectContents("/target", data_.substr(0, static_cast<size_t>(length)));
    ExpectContents("/source", data_);
    uint64_t size;
    ASSERT_LEVELDB_OK(base_->GetFileSize("/target", &size));
    EXPECT_EQ(length, size);
    uint64_t requested = 0;
    for (size_t n : env.read_requests) {
      EXPECT_GT(n, 0);
      EXPECT_LE(n, 8192);
      requested += n;
    }
    // No read may probe beyond the prefix, including an extra EOF read.
    EXPECT_EQ(length, requested);
    if (length == 0) EXPECT_EQ(0, env.read_calls);
    EXPECT_EQ(0, env.link_calls);
    EXPECT_EQ(1, env.sync_calls);
    EXPECT_EQ(1, env.close_calls);
    EXPECT_EQ(0, env.active_readers);
    EXPECT_EQ(0, env.active_writers);
    ASSERT_LEVELDB_OK(base_->RemoveFile("/target"));
  }
}

TEST_F(CopyFilePrefixTest, ShortReadsAreNotEof) {
  env_.max_read_size = 7;
  ASSERT_LEVELDB_OK(CopyFilePrefix(&env_, "/source", "/target", 8201));
  ExpectContents("/target", data_.substr(0, 8201));
  EXPECT_GT(env_.read_calls, 8201 / 7);
  EXPECT_EQ(0, env_.link_calls);
}

TEST_F(CopyFilePrefixTest, AppendsDuringCopyAreExcluded) {
  bool appended = false;
  const std::string tail(10000, 'x');
  env_.before_read = [&] {
    if (env_.read_calls != 2) return;
    // Interleave an append after the first copied chunk, without timing races.
    WritableFile* file;
    ASSERT_LEVELDB_OK(base_->NewAppendableFile("/source", &file));
    std::unique_ptr<WritableFile> append_file(file);
    ASSERT_LEVELDB_OK(append_file->Append(tail));
    ASSERT_LEVELDB_OK(append_file->Flush());
    ASSERT_LEVELDB_OK(append_file->Close());
    appended = true;
  };
  ASSERT_LEVELDB_OK(CopyFilePrefix(&env_, "/source", "/target", data_.size()));
  ASSERT_TRUE(appended);
  ExpectContents("/source", data_ + tail);
  ExpectContents("/target", data_);
  ASSERT_LEVELDB_OK(base_->RemoveFile("/source"));
  ExpectContents("/target", data_);
}

TEST_F(CopyFilePrefixTest, EarlyEofRemovesPartialTarget) {
  for (size_t source_size : {0, 25000}) {
    SCOPED_TRACE(source_size);
    const std::string source = data_.substr(0, source_size);
    ASSERT_LEVELDB_OK(WriteStringToFile(base_.get(), source, "/source"));
    CopyTestEnv env(base_.get());
    const Status status =
        CopyFilePrefix(&env, "/source", "/target", source_size + 1);
    EXPECT_TRUE(status.IsIOError());
    EXPECT_NE(std::string::npos, status.ToString().find("unexpected EOF"));
    EXPECT_FALSE(base_->FileExists("/target"));
    ExpectContents("/source", source);
    EXPECT_EQ(0, env.sync_calls);
    EXPECT_EQ(1, env.close_calls);
    EXPECT_EQ(1, env.remove_calls);
    EXPECT_EQ(0, env.active_readers);
    EXPECT_EQ(0, env.active_writers);
    EXPECT_FALSE(env.removed_with_open_file);
  }
}

TEST_F(CopyFilePrefixTest, LargeLengthsDoNotWrapOrMeanUntilEof) {
  const uint64_t lengths[] = {(uint64_t{1} << 32) + 1,
                              std::numeric_limits<uint64_t>::max()};
  for (uint64_t length : lengths) {
    SCOPED_TRACE(length);
    CopyTestEnv env(base_.get());
    EXPECT_TRUE(CopyFilePrefix(&env, "/source", "/target", length).IsIOError());
    EXPECT_FALSE(base_->FileExists("/target"));
    ExpectContents("/source", data_);
    for (size_t n : env.read_requests) EXPECT_LE(n, 8192);
  }
}

TEST_F(CopyFilePrefixTest, ZeroLengthStillRequiresSource) {
  EXPECT_FALSE(CopyFilePrefix(&env_, "/missing", "/target", 0).ok());
  EXPECT_FALSE(base_->FileExists("/target"));
  EXPECT_EQ(0, env_.create_calls);
  EXPECT_EQ(0, env_.remove_calls);
}

TEST_F(CopyFilePrefixTest, ExistingTargetAndSamePathArePreserved) {
  ASSERT_LEVELDB_OK(WriteStringToFile(base_.get(), "keep", "/target"));
  EXPECT_TRUE(
      CopyFilePrefix(&env_, "/source", "/target", 0).IsInvalidArgument());
  EXPECT_TRUE(
      CopyFilePrefix(&env_, "/source", "/source", 0).IsInvalidArgument());
  ExpectContents("/source", data_);
  ExpectContents("/target", "keep");
  EXPECT_EQ(0, env_.create_calls);
  EXPECT_EQ(0, env_.remove_calls);
}

TEST_F(CopyFilePrefixTest, FailuresCloseFilesAndRemovePartialTarget) {
  for (CopyTestEnv::Failure failure :
       {CopyTestEnv::kOpen, CopyTestEnv::kCreate, CopyTestEnv::kRead,
        CopyTestEnv::kAppend, CopyTestEnv::kSync, CopyTestEnv::kClose}) {
    SCOPED_TRACE(failure);
    CopyTestEnv env(base_.get());
    env.failure = failure;
    const Status status = CopyFilePrefix(&env, "/source", "/target", 9000);
    EXPECT_EQ(CopyTestEnv::Error(failure).ToString(), status.ToString());
    EXPECT_FALSE(base_->FileExists("/target"));
    ExpectContents("/source", data_);
    const bool opened_target =
        failure != CopyTestEnv::kOpen && failure != CopyTestEnv::kCreate;
    EXPECT_EQ(opened_target ? 1 : 0, env.close_calls);
    EXPECT_EQ(opened_target ? 1 : 0, env.remove_calls);
    EXPECT_EQ(0, env.active_readers);
    EXPECT_EQ(0, env.active_writers);
    EXPECT_FALSE(env.removed_with_open_file);
  }
}

TEST_F(CopyFilePrefixTest, EofErrorSurvivesCloseAndCleanupFailures) {
  for (bool fail_remove : {false, true}) {
    SCOPED_TRACE(fail_remove);
    CopyTestEnv env(base_.get());
    env.fail_close = true;
    env.fail_remove = fail_remove;
    const Status status =
        CopyFilePrefix(&env, "/source", "/target", data_.size() + 1);
    EXPECT_TRUE(status.IsIOError());
    EXPECT_NE(std::string::npos, status.ToString().find("unexpected EOF"));
    EXPECT_EQ(std::string::npos,
              status.ToString().find("injected close error"));
    EXPECT_EQ(fail_remove, status.ToString().find("injected remove error") !=
                               std::string::npos);
    EXPECT_EQ(fail_remove, base_->FileExists("/target"));
    EXPECT_EQ(1, env.close_calls);
    EXPECT_EQ(0, env.active_readers);
    EXPECT_EQ(0, env.active_writers);
    ExpectContents("/source", data_);
    if (fail_remove) ASSERT_LEVELDB_OK(base_->RemoveFile("/target"));
  }
}

class FinalizeEnv : public EnvWrapper {
 public:
  FinalizeEnv() : EnvWrapper(nullptr) {}
  MOCK_METHOD(Status, CreateDir, (const std::string& dir), (override));
  MOCK_METHOD(Status, GetChildren,
              (const std::string& dir, std::vector<std::string>* children),
              (override));
  MOCK_METHOD(Status, SyncDir, (const std::string& dir), (override));
  MOCK_METHOD(Status, RenameFile,
              (const std::string& src, const std::string& target), (override));
  MOCK_METHOD(Status, RemoveFile, (const std::string& path), (override));
  MOCK_METHOD(Status, RemoveDir, (const std::string& path), (override));
};

class FinalizeCheckpointTest : public testing::Test {
 protected:
  void ExpectListing(const std::string& parent = "/backup",
                     const std::vector<std::string>& children = {".", "..",
                                                                 "staging"}) {
    EXPECT_CALL(env_, GetChildren(parent, testing::_))
        .WillOnce(testing::DoAll(testing::SetArgPointee<1>(children),
                                 testing::Return(Status::OK())));
  }

  Status Finalize() {
    return FinalizeCheckpointDirectory(&env_, "/backup", "staging",
                                       "checkpoint", &renamed_);
  }

  testing::StrictMock<FinalizeEnv> env_;
  bool renamed_ = true;
};

TEST_F(FinalizeCheckpointTest, SyncRenameAndParentSyncInOrder) {
  testing::InSequence order;
  ExpectListing();
  EXPECT_CALL(env_, SyncDir("/backup/staging"))
      .WillOnce(testing::Return(Status::OK()));
  EXPECT_CALL(env_, RenameFile("/backup/staging", "/backup/checkpoint"))
      .WillOnce(testing::Return(Status::OK()));
  EXPECT_CALL(env_, SyncDir("/backup")).WillOnce([&](const std::string&) {
    EXPECT_TRUE(renamed_);
    return Status::OK();
  });
  ASSERT_LEVELDB_OK(Finalize());
  EXPECT_TRUE(renamed_);
}

TEST_F(FinalizeCheckpointTest, ListingFailureDoesNotMutatePaths) {
  const Status error = Status::IOError("listing failure");
  EXPECT_CALL(env_, GetChildren("/backup", testing::_))
      .WillOnce(testing::Return(error));
  EXPECT_EQ(error.ToString(), Finalize().ToString());
  EXPECT_FALSE(renamed_);
}

TEST_F(FinalizeCheckpointTest, ExistingDestinationIsRejected) {
  ExpectListing("/backup", {"staging", "checkpoint"});
  EXPECT_TRUE(Finalize().IsInvalidArgument());
  EXPECT_FALSE(renamed_);
}

TEST_F(FinalizeCheckpointTest, StagingSyncFailureDoesNotRenameOrRemove) {
  for (const Status& error :
       {Status::NotSupported("sync unsupported"),
        Status::NotFound("missing staging"), Status::IOError("sync failure")}) {
    SCOPED_TRACE(error.ToString());
    renamed_ = true;
    ExpectListing();
    EXPECT_CALL(env_, SyncDir("/backup/staging"))
        .WillOnce(testing::Return(error));
    EXPECT_EQ(error.ToString(), Finalize().ToString());
    EXPECT_FALSE(renamed_);
  }
}

TEST_F(FinalizeCheckpointTest, RenameFailureDoesNotSyncParentOrRemove) {
  testing::InSequence order;
  ExpectListing();
  EXPECT_CALL(env_, SyncDir("/backup/staging"))
      .WillOnce(testing::Return(Status::OK()));
  const Status error = Status::IOError("rename failure");
  EXPECT_CALL(env_, RenameFile("/backup/staging", "/backup/checkpoint"))
      .WillOnce(testing::Return(error));
  EXPECT_EQ(error.ToString(), Finalize().ToString());
  EXPECT_FALSE(renamed_);
}

TEST_F(FinalizeCheckpointTest, ParentSyncFailureReportsRenameWithoutRemoval) {
  testing::InSequence order;
  ExpectListing();
  EXPECT_CALL(env_, SyncDir("/backup/staging"))
      .WillOnce(testing::Return(Status::OK()));
  EXPECT_CALL(env_, RenameFile("/backup/staging", "/backup/checkpoint"))
      .WillOnce(testing::Return(Status::OK()));
  const Status error = Status::IOError("parent sync failure");
  EXPECT_CALL(env_, SyncDir("/backup")).WillOnce(testing::Return(error));
  EXPECT_EQ(error.ToString(), Finalize().ToString());
  EXPECT_TRUE(renamed_);
}

TEST_F(FinalizeCheckpointTest, InvalidNamesDoNotTouchFilesystem) {
  const std::string invalid_names[] = {"",
                                       ".",
                                       "..",
                                       "nested/path",
                                       "nested\\path",
                                       std::string("nul\0name", 8)};
  for (const std::string& name : invalid_names) {
    SCOPED_TRACE(name);
    renamed_ = true;
    EXPECT_TRUE(FinalizeCheckpointDirectory(&env_, "/backup", name,
                                            "checkpoint", &renamed_)
                    .IsInvalidArgument());
    EXPECT_FALSE(renamed_);
    renamed_ = true;
    EXPECT_TRUE(FinalizeCheckpointDirectory(&env_, "/backup", "staging", name,
                                            &renamed_)
                    .IsInvalidArgument());
    EXPECT_FALSE(renamed_);
  }
  EXPECT_TRUE(
      FinalizeCheckpointDirectory(&env_, "", "staging", "checkpoint", &renamed_)
          .IsInvalidArgument());
  EXPECT_TRUE(FinalizeCheckpointDirectory(&env_, std::string("nul\0parent", 10),
                                          "staging", "checkpoint", &renamed_)
                  .IsInvalidArgument());
  EXPECT_TRUE(
      FinalizeCheckpointDirectory(&env_, "/backup", "same", "same", &renamed_)
          .IsInvalidArgument());
  EXPECT_TRUE(FinalizeCheckpointDirectory(&env_, "/backup", "staging",
                                          "checkpoint", nullptr)
                  .IsInvalidArgument());
}

TEST_F(FinalizeCheckpointTest, RootRelativeAndTrailingSlashParents) {
  for (const std::string& parent : {"/", ".", "/backup/"}) {
    SCOPED_TRACE(parent);
    testing::InSequence order;
    ExpectListing(parent);
    const std::string prefix = parent.back() == '/' ? parent : parent + "/";
    EXPECT_CALL(env_, SyncDir(prefix + "staging"))
        .WillOnce(testing::Return(Status::OK()));
    EXPECT_CALL(env_, RenameFile(prefix + "staging", prefix + "checkpoint"))
        .WillOnce(testing::Return(Status::OK()));
    EXPECT_CALL(env_, SyncDir(parent)).WillOnce(testing::Return(Status::OK()));
    ASSERT_LEVELDB_OK(FinalizeCheckpointDirectory(&env_, parent, "staging",
                                                  "checkpoint", &renamed_));
    EXPECT_TRUE(renamed_);
  }
}

class CreateCheckpointTest : public testing::Test {
 protected:
  void ExpectListing(const std::string& path,
                     const std::vector<std::string>& children) {
    EXPECT_CALL(env_, GetChildren(path, testing::_))
        .WillOnce(testing::DoAll(testing::SetArgPointee<1>(children),
                                 testing::Return(Status::OK())));
  }

  void ExpectCreate(const Status& populate_status) {
    ExpectListing("/backup", {});
    EXPECT_CALL(env_, CreateDir("/backup/staging"))
        .WillOnce(testing::Return(Status::OK()));
    EXPECT_CALL(populate_, Call("/backup/staging"))
        .WillOnce(testing::Return(populate_status));
  }

  void ExpectCleanup() {
    ExpectListing("/backup/staging", {".", "..", "000001.ldb", "CURRENT"});
    EXPECT_CALL(env_, RemoveFile("/backup/staging/000001.ldb"))
        .WillOnce(testing::Return(Status::OK()));
    EXPECT_CALL(env_, RemoveFile("/backup/staging/CURRENT"))
        .WillOnce(testing::Return(Status::OK()));
    EXPECT_CALL(env_, RemoveDir("/backup/staging"))
        .WillOnce(testing::Return(Status::OK()));
    EXPECT_CALL(env_, SyncDir("/backup"))
        .WillOnce(testing::Return(Status::OK()));
  }

  Status Create() {
    return CreateCheckpointDirectory(&env_, "/backup", "staging", "checkpoint",
                                     populate_.AsStdFunction(), &renamed_);
  }

  testing::StrictMock<FinalizeEnv> env_;
  testing::StrictMock<testing::MockFunction<Status(const std::string&)>>
      populate_;
  bool renamed_ = true;
};

TEST_F(CreateCheckpointTest, SuccessPublishesWithoutCleanup) {
  testing::InSequence order;
  ExpectCreate(Status::OK());
  ExpectListing("/backup", {"staging"});
  EXPECT_CALL(env_, SyncDir("/backup/staging"))
      .WillOnce(testing::Return(Status::OK()));
  EXPECT_CALL(env_, RenameFile("/backup/staging", "/backup/checkpoint"))
      .WillOnce(testing::Return(Status::OK()));
  EXPECT_CALL(env_, SyncDir("/backup")).WillOnce(testing::Return(Status::OK()));
  ASSERT_LEVELDB_OK(Create());
  EXPECT_TRUE(renamed_);
}

TEST_F(CreateCheckpointTest, ExistingNamesNeverBecomeOwned) {
  for (const std::string& name : {"staging", "checkpoint"}) {
    SCOPED_TRACE(name);
    ExpectListing("/backup", {name});
    EXPECT_TRUE(Create().IsInvalidArgument());
    EXPECT_FALSE(renamed_);
  }
}

TEST_F(CreateCheckpointTest, ListingAndCreateFailuresDoNotCleanAnything) {
  const Status error = Status::IOError("injected setup failure");
  EXPECT_CALL(env_, GetChildren("/backup", testing::_))
      .WillOnce(testing::Return(error));
  EXPECT_EQ(error.ToString(), Create().ToString());
  EXPECT_FALSE(renamed_);

  ExpectListing("/backup", {});
  EXPECT_CALL(env_, CreateDir("/backup/staging"))
      .WillOnce(testing::Return(error));
  EXPECT_EQ(error.ToString(), Create().ToString());
  EXPECT_FALSE(renamed_);
}

TEST_F(CreateCheckpointTest,
       PopulateFailureCleansFilesAndPreservesOriginalStatus) {
  testing::InSequence order;
  const Status error = Status::Corruption("injected populate failure");
  ExpectCreate(error);
  ExpectCleanup();
  EXPECT_EQ(error.ToString(), Create().ToString());
  EXPECT_FALSE(renamed_);
}

TEST_F(CreateCheckpointTest, FinalizeFailuresBeforePublicationCleanStaging) {
  for (int failure = 0; failure < 3; ++failure) {
    SCOPED_TRACE(failure);
    testing::InSequence order;
    const Status error = Status::IOError("injected finalization failure");
    ExpectCreate(Status::OK());
    if (failure == 0) {
      EXPECT_CALL(env_, GetChildren("/backup", testing::_))
          .WillOnce(testing::Return(error));
    } else {
      ExpectListing("/backup", {"staging"});
      EXPECT_CALL(env_, SyncDir("/backup/staging"))
          .WillOnce(testing::Return(failure == 1 ? error : Status::OK()));
      if (failure == 2) {
        EXPECT_CALL(env_, RenameFile("/backup/staging", "/backup/checkpoint"))
            .WillOnce(testing::Return(error));
      }
    }
    ExpectCleanup();
    EXPECT_EQ(error.ToString(), Create().ToString());
    EXPECT_FALSE(renamed_);
  }
}

TEST_F(CreateCheckpointTest, ParentSyncFailureNeverCleansPublishedDirectory) {
  testing::InSequence order;
  const Status error = Status::IOError("injected parent sync failure");
  ExpectCreate(Status::OK());
  ExpectListing("/backup", {"staging"});
  EXPECT_CALL(env_, SyncDir("/backup/staging"))
      .WillOnce(testing::Return(Status::OK()));
  EXPECT_CALL(env_, RenameFile("/backup/staging", "/backup/checkpoint"))
      .WillOnce(testing::Return(Status::OK()));
  EXPECT_CALL(env_, SyncDir("/backup")).WillOnce(testing::Return(error));
  EXPECT_EQ(error.ToString(), Create().ToString());
  EXPECT_TRUE(renamed_);
}

TEST_F(CreateCheckpointTest, RemovalFailureContinuesAndReportsBothErrors) {
  testing::InSequence order;
  ExpectCreate(Status::Corruption("populate failed"));
  ExpectListing("/backup/staging", {"first", "second"});
  EXPECT_CALL(env_, RemoveFile("/backup/staging/first"))
      .WillOnce(testing::Return(Status::IOError("first removal failed")));
  EXPECT_CALL(env_, RemoveFile("/backup/staging/second"))
      .WillOnce(testing::Return(Status::OK()));
  EXPECT_CALL(env_, RemoveDir("/backup/staging"))
      .WillOnce(testing::Return(Status::IOError("directory still not empty")));
  const Status result = Create();
  EXPECT_TRUE(result.IsIOError());
  EXPECT_NE(std::string::npos, result.ToString().find("populate failed"));
  EXPECT_NE(std::string::npos, result.ToString().find("first removal failed"));
  EXPECT_NE(std::string::npos, result.ToString().find("/backup/staging"));
  EXPECT_FALSE(renamed_);
}

TEST_F(CreateCheckpointTest,
       CleanupListingDirectoryAndSyncFailuresAreReported) {
  for (int failure = 0; failure < 3; ++failure) {
    SCOPED_TRACE(failure);
    testing::InSequence order;
    ExpectCreate(Status::Corruption("populate failed"));
    const Status error = Status::IOError("cleanup failed");
    if (failure == 0) {
      EXPECT_CALL(env_, GetChildren("/backup/staging", testing::_))
          .WillOnce(testing::Return(error));
    } else {
      ExpectListing("/backup/staging", {});
      EXPECT_CALL(env_, RemoveDir("/backup/staging"))
          .WillOnce(testing::Return(failure == 1 ? error : Status::OK()));
      if (failure == 2) {
        EXPECT_CALL(env_, SyncDir("/backup")).WillOnce(testing::Return(error));
      }
    }
    const Status result = Create();
    EXPECT_TRUE(result.IsIOError());
    EXPECT_NE(std::string::npos, result.ToString().find("populate failed"));
    EXPECT_NE(std::string::npos, result.ToString().find("cleanup failed"));
    EXPECT_FALSE(renamed_);
  }
}

TEST_F(CreateCheckpointTest, InvalidChildListingCannotEscapeStaging) {
  for (const std::string& child :
       {std::string(""), std::string("../outside"), std::string("/outside"),
        std::string("nested/file"), std::string("nested\\file"),
        std::string("nul\0name", 8)}) {
    SCOPED_TRACE(child);
    testing::InSequence order;
    ExpectCreate(Status::Corruption("populate failed"));
    ExpectListing("/backup/staging", {"safe-file", child});
    const Status result = Create();
    EXPECT_TRUE(result.IsIOError());
    EXPECT_NE(std::string::npos, result.ToString().find("populate failed"));
    EXPECT_NE(std::string::npos,
              result.ToString().find("invalid staging child"));
  }
}

TEST_F(CreateCheckpointTest, AlreadyMissingFilesDoNotHideOriginalFailure) {
  testing::InSequence order;
  const Status error = Status::Corruption("populate failed");
  ExpectCreate(error);
  ExpectListing("/backup/staging", {"already-removed"});
  EXPECT_CALL(env_, RemoveFile("/backup/staging/already-removed"))
      .WillOnce(testing::Return(Status::NotFound("already removed")));
  EXPECT_CALL(env_, RemoveDir("/backup/staging"))
      .WillOnce(testing::Return(Status::OK()));
  EXPECT_CALL(env_, SyncDir("/backup")).WillOnce(testing::Return(Status::OK()));
  EXPECT_EQ(error.ToString(), Create().ToString());
  EXPECT_FALSE(renamed_);
}

TEST_F(CreateCheckpointTest, InvalidArgumentsDoNotTouchFilesystem) {
  const auto populate = populate_.AsStdFunction();
  for (const std::string& name :
       {"", ".", "..", "nested/path", "nested\\path"}) {
    SCOPED_TRACE(name);
    EXPECT_TRUE(CreateCheckpointDirectory(&env_, "/backup", name, "checkpoint",
                                          populate, &renamed_)
                    .IsInvalidArgument());
    EXPECT_FALSE(renamed_);
    EXPECT_TRUE(CreateCheckpointDirectory(&env_, "/backup", "staging", name,
                                          populate, &renamed_)
                    .IsInvalidArgument());
    EXPECT_FALSE(renamed_);
  }
  EXPECT_TRUE(CreateCheckpointDirectory(&env_, "", "staging", "checkpoint",
                                        populate, &renamed_)
                  .IsInvalidArgument());
  EXPECT_TRUE(CreateCheckpointDirectory(&env_, "/backup", "same", "same",
                                        populate, &renamed_)
                  .IsInvalidArgument());
  EXPECT_TRUE(CreateCheckpointDirectory(&env_, "/backup", "staging",
                                        "checkpoint", {}, &renamed_)
                  .IsInvalidArgument());
  EXPECT_TRUE(CreateCheckpointDirectory(&env_, "/backup", "staging",
                                        "checkpoint", populate, nullptr)
                  .IsInvalidArgument());
}

}  // namespace
}  // namespace leveldb
