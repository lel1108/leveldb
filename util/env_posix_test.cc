// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "db/checkpoint.h"
#include "gtest/gtest.h"
#include "leveldb/env.h"
#include "port/port.h"
#include "util/env_posix_test_helper.h"
#include "util/env_util.h"
#include "util/testutil.h"

#if HAVE_O_CLOEXEC

namespace {

// Exit codes for the helper process spawned by TestCloseOnExec* tests.
// Useful for debugging test failures.
constexpr int kTextCloseOnExecHelperExecFailedCode = 61;
constexpr int kTextCloseOnExecHelperDup2FailedCode = 62;
constexpr int kTextCloseOnExecHelperFoundOpenFdCode = 63;

// Global set by main() and read in TestCloseOnExec.
//
// The argv[0] value is stored in a std::vector instead of a std::string because
// std::string does not return a mutable pointer to its buffer until C++17.
//
// The vector stores the string pointed to by argv[0], plus the trailing null.
std::vector<char>* GetArgvZero() {
  static std::vector<char> program_name;
  return &program_name;
}

// Command-line switch used to run this test as the CloseOnExecSwitch helper.
static const char kTestCloseOnExecSwitch[] = "--test-close-on-exec-helper";

// Executed in a separate process by TestCloseOnExec* tests.
//
// main() delegates to this function when the test executable is launched with
// a special command-line switch. TestCloseOnExec* tests fork()+exec() the test
// executable and pass the special command-line switch.
//

// main() delegates to this function when the test executable is launched with
// a special command-line switch. TestCloseOnExec* tests fork()+exec() the test
// executable and pass the special command-line switch.
//
// When main() delegates to this function, the process probes whether a given
// file descriptor is open, and communicates the result via its exit code.
int TestCloseOnExecHelperMain(char* pid_arg) {
  int fd = std::atoi(pid_arg);
  // When given the same file descriptor twice, dup2() returns -1 if the
  // file descriptor is closed, or the given file descriptor if it is open.
  if (::dup2(fd, fd) == fd) {
    std::fprintf(stderr, "Unexpected open fd %d\n", fd);
    return kTextCloseOnExecHelperFoundOpenFdCode;
  }
  // Double-check that dup2() is saying the file descriptor is closed.
  if (errno != EBADF) {
    std::fprintf(stderr, "Unexpected errno after calling dup2 on fd %d: %s\n",
                 fd, std::strerror(errno));
    return kTextCloseOnExecHelperDup2FailedCode;
  }
  return 0;
}

// File descriptors are small non-negative integers.
//
// Returns void so the implementation can use ASSERT_EQ.
void GetMaxFileDescriptor(int* result_fd) {
  // Get the maximum file descriptor number.
  ::rlimit fd_rlimit;
  ASSERT_EQ(0, ::getrlimit(RLIMIT_NOFILE, &fd_rlimit));
  *result_fd = fd_rlimit.rlim_cur;
}

// Iterates through all possible FDs and returns the currently open ones.
//
// Returns void so the implementation can use ASSERT_EQ.
void GetOpenFileDescriptors(std::unordered_set<int>* open_fds) {
  int max_fd = 0;
  GetMaxFileDescriptor(&max_fd);

  for (int fd = 0; fd < max_fd; ++fd) {
    if (::dup2(fd, fd) != fd) {
      // When given the same file descriptor twice, dup2() returns -1 if the
      // file descriptor is closed, or the given file descriptor if it is open.
      //
      // Double-check that dup2() is saying the fd is closed.
      ASSERT_EQ(EBADF, errno)
          << "dup2() should set errno to EBADF on closed file descriptors";
      continue;
    }
    open_fds->insert(fd);
  }
}

// Finds an FD open since a previous call to GetOpenFileDescriptors().
//
// |baseline_open_fds| is the result of a previous GetOpenFileDescriptors()
// call. Assumes that exactly one FD was opened since that call.
//
// Returns void so the implementation can use ASSERT_EQ.
void GetNewlyOpenedFileDescriptor(
    const std::unordered_set<int>& baseline_open_fds, int* result_fd) {
  std::unordered_set<int> open_fds;
  GetOpenFileDescriptors(&open_fds);
  for (int fd : baseline_open_fds) {
    ASSERT_EQ(1, open_fds.count(fd))
        << "Previously opened file descriptor was closed during test setup";
    open_fds.erase(fd);
  }
  ASSERT_EQ(1, open_fds.size())
      << "Expected exactly one newly opened file descriptor during test setup";
  *result_fd = *open_fds.begin();
}

// Check that a fork()+exec()-ed child process does not have an extra open FD.
void CheckCloseOnExecDoesNotLeakFDs(
    const std::unordered_set<int>& baseline_open_fds) {
  // Prepare the argument list for the child process.
  // execv() wants mutable buffers.
  char switch_buffer[sizeof(kTestCloseOnExecSwitch)];
  std::memcpy(switch_buffer, kTestCloseOnExecSwitch,
              sizeof(kTestCloseOnExecSwitch));

  int probed_fd;
  GetNewlyOpenedFileDescriptor(baseline_open_fds, &probed_fd);
  std::string fd_string = std::to_string(probed_fd);
  std::vector<char> fd_buffer(fd_string.begin(), fd_string.end());
  fd_buffer.emplace_back('\0');

  // The helper process is launched with the command below.
  //      env_posix_tests --test-close-on-exec-helper 3
  char* child_argv[] = {GetArgvZero()->data(), switch_buffer, fd_buffer.data(),
                        nullptr};

  constexpr int kForkInChildProcessReturnValue = 0;
  int child_pid = fork();
  if (child_pid == kForkInChildProcessReturnValue) {
    ::execv(child_argv[0], child_argv);
    std::fprintf(stderr, "Error spawning child process: %s\n", strerror(errno));
    std::exit(kTextCloseOnExecHelperExecFailedCode);
  }

  int child_status = 0;
  ASSERT_EQ(child_pid, ::waitpid(child_pid, &child_status, 0));
  ASSERT_TRUE(WIFEXITED(child_status))
      << "The helper process did not exit with an exit code";
  ASSERT_EQ(0, WEXITSTATUS(child_status))
      << "The helper process encountered an error";
}

}  // namespace

#endif  // HAVE_O_CLOEXEC

namespace leveldb {

static const int kReadOnlyFileLimit = 4;
static const int kMMapLimit = 4;

class EnvPosixTest : public testing::Test {
 public:
  static void SetFileLimits(int read_only_file_limit, int mmap_limit) {
    EnvPosixTestHelper::SetReadOnlyFDLimit(read_only_file_limit);
    EnvPosixTestHelper::SetReadOnlyMMapLimit(mmap_limit);
  }

  EnvPosixTest() : env_(Env::Default()) {}

  Env* env_;
};

class EnvPosixLinkTest : public EnvPosixTest {
 protected:
  void SetUp() override {
    std::string test_dir;
    ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
    std::string dir_template = test_dir + "/link-test-XXXXXX";
    char* dir = ::mkdtemp(&dir_template[0]);
    ASSERT_NE(nullptr, dir) << std::strerror(errno);
    directories_.emplace_back(dir);
    src_ = directories_[0] + "/source";
    target_ = directories_[0] + "/target";
  }

  void TearDown() override {
    for (const std::string& dir : directories_) {
      std::vector<std::string> children;
      EXPECT_LEVELDB_OK(env_->GetChildren(dir, &children));
      for (const std::string& child : children) {
        if (child != "." && child != "..") {
          EXPECT_LEVELDB_OK(env_->RemoveFile(dir + "/" + child));
        }
      }
      EXPECT_LEVELDB_OK(env_->RemoveDir(dir));
    }
  }

  std::vector<std::string> directories_;
  std::string src_;
  std::string target_;
};

TEST_F(EnvPosixLinkTest, SharesInodeAndSurvivesSourceRemoval) {
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "contents", src_));
  ASSERT_LEVELDB_OK(env_->LinkFile(src_, target_));
  struct stat src_stat;
  struct stat target_stat;
  ASSERT_EQ(0, ::stat(src_.c_str(), &src_stat));
  ASSERT_EQ(0, ::stat(target_.c_str(), &target_stat));
  EXPECT_EQ(src_stat.st_dev, target_stat.st_dev);
  EXPECT_EQ(src_stat.st_ino, target_stat.st_ino);
  EXPECT_EQ(2, src_stat.st_nlink);
  ASSERT_LEVELDB_OK(env_->RemoveFile(src_));
  EXPECT_FALSE(env_->FileExists(src_));
  std::string contents;
  ASSERT_LEVELDB_OK(ReadFileToString(env_, target_, &contents));
  EXPECT_EQ("contents", contents);
}

TEST_F(EnvPosixLinkTest, MissingSource) {
  EXPECT_TRUE(env_->LinkFile(src_, target_).IsNotFound());
  EXPECT_FALSE(env_->FileExists(target_));
}

TEST_F(EnvPosixLinkTest, LinkOrCopyUsesHardLinkWhenAvailable) {
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "contents", src_));
  ASSERT_LEVELDB_OK(LinkOrCopyFile(env_, src_, target_));
  struct stat src_stat;
  struct stat target_stat;
  ASSERT_EQ(0, ::stat(src_.c_str(), &src_stat));
  ASSERT_EQ(0, ::stat(target_.c_str(), &target_stat));
  EXPECT_EQ(src_stat.st_dev, target_stat.st_dev);
  EXPECT_EQ(src_stat.st_ino, target_stat.st_ino);
}

TEST_F(EnvPosixLinkTest, ExistingTargetIsNotOverwritten) {
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "source data", src_));
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "keep target", target_));
  EXPECT_TRUE(env_->LinkFile(src_, target_).IsIOError());
  std::string contents;
  ASSERT_LEVELDB_OK(ReadFileToString(env_, src_, &contents));
  EXPECT_EQ("source data", contents);
  ASSERT_LEVELDB_OK(ReadFileToString(env_, target_, &contents));
  EXPECT_EQ("keep target", contents);
}

TEST_F(EnvPosixLinkTest, CopyFilePrefixCreatesIndependentFile) {
  const std::string data(20000, 'a');
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, data, src_));
  ASSERT_LEVELDB_OK(CopyFilePrefix(env_, src_, target_, 8193));
  struct stat src_stat;
  struct stat target_stat;
  ASSERT_EQ(0, ::stat(src_.c_str(), &src_stat));
  ASSERT_EQ(0, ::stat(target_.c_str(), &target_stat));
  EXPECT_NE(src_stat.st_ino, target_stat.st_ino);
  EXPECT_EQ(8193, target_stat.st_size);
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "changed", src_));
  ASSERT_LEVELDB_OK(env_->RemoveFile(src_));
  std::string contents;
  ASSERT_LEVELDB_OK(ReadFileToString(env_, target_, &contents));
  EXPECT_EQ(data.substr(0, 8193), contents);
}

TEST_F(EnvPosixLinkTest, SamePathIsNotOverwritten) {
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "contents", src_));
  EXPECT_TRUE(env_->LinkFile(src_, src_).IsIOError());
  std::string contents;
  ASSERT_LEVELDB_OK(ReadFileToString(env_, src_, &contents));
  EXPECT_EQ("contents", contents);
}

TEST_F(EnvPosixLinkTest, MissingTargetDirectory) {
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "contents", src_));
  EXPECT_TRUE(env_->LinkFile(src_, target_ + "/missing").IsNotFound());
  EXPECT_FALSE(env_->FileExists(target_));
  std::string contents;
  ASSERT_LEVELDB_OK(ReadFileToString(env_, src_, &contents));
  EXPECT_EQ("contents", contents);
}

TEST_F(EnvPosixLinkTest, CrossDeviceNotSupported) {
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "contents", src_));
  struct stat src_stat;
  ASSERT_EQ(0, ::stat(src_.c_str(), &src_stat));

  // Use a real second filesystem when available, without requiring mounts or
  // elevated privileges. Some POSIX hosts have only one writable filesystem.
  for (const char* parent : {"/tmp", "/var/tmp", "/dev/shm"}) {
    struct stat parent_stat;
    if (::stat(parent, &parent_stat) != 0 ||
        src_stat.st_dev == parent_stat.st_dev) {
      continue;
    }
    std::string dir_template =
        std::string(parent) + "/leveldb-link-test-XXXXXX";
    char* dir = ::mkdtemp(&dir_template[0]);
    if (dir == nullptr) {
      continue;
    }
    directories_.emplace_back(dir);
    const std::string cross_device_target = directories_.back() + "/target";
    Status status = env_->LinkFile(src_, cross_device_target);
    EXPECT_TRUE(status.IsNotSupportedError()) << status.ToString();
    EXPECT_FALSE(env_->FileExists(cross_device_target));
    std::string contents;
    ASSERT_LEVELDB_OK(ReadFileToString(env_, src_, &contents));
    EXPECT_EQ("contents", contents);
    ASSERT_LEVELDB_OK(LinkOrCopyFile(env_, src_, cross_device_target));
    struct stat target_stat;
    ASSERT_EQ(0, ::stat(cross_device_target.c_str(), &target_stat));
    EXPECT_NE(src_stat.st_dev, target_stat.st_dev);
    ASSERT_LEVELDB_OK(env_->RemoveFile(src_));
    ASSERT_LEVELDB_OK(ReadFileToString(env_, cross_device_target, &contents));
    EXPECT_EQ("contents", contents);
    return;
  }
  GTEST_SKIP() << "No writable directory on a different filesystem";
}

TEST_F(EnvPosixLinkTest, SyncDirAcceptsDirectory) {
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "contents", src_));
  ASSERT_LEVELDB_OK(env_->SyncDir(directories_[0]));
}

TEST_F(EnvPosixLinkTest, SyncDirRejectsMissingAndRegularFile) {
  EXPECT_TRUE(env_->SyncDir(target_).IsNotFound());
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "contents", src_));
  EXPECT_TRUE(env_->SyncDir(src_).IsIOError());
}

class EnvPosixDirectoryTest : public EnvPosixLinkTest {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(EnvPosixLinkTest::SetUp());
    parent_ = directories_[0];
    staging_ = parent_ + "/staging";
    checkpoint_ = parent_ + "/checkpoint";
    ASSERT_LEVELDB_OK(env_->CreateDir(staging_));
  }

  void TearDown() override {
    // Remove only the two known test directories, without following symlinks.
    for (const std::string& dir : {staging_, checkpoint_}) {
      struct stat dir_stat;
      if (::lstat(dir.c_str(), &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
        continue;
      }
      std::vector<std::string> children;
      EXPECT_LEVELDB_OK(env_->GetChildren(dir, &children));
      for (const std::string& child : children) {
        if (child != "." && child != "..") {
          EXPECT_LEVELDB_OK(env_->RemoveFile(dir + "/" + child));
        }
      }
      EXPECT_LEVELDB_OK(env_->RemoveDir(dir));
    }
    EnvPosixLinkTest::TearDown();
  }

  void PrepareData() {
    ASSERT_LEVELDB_OK(WriteStringToFile(env_, "checkpoint contents", src_));
    ASSERT_LEVELDB_OK(CopyFileContents(env_, src_, staging_ + "/data"));
  }

  std::string parent_;
  std::string staging_;
  std::string checkpoint_;
};

TEST_F(EnvPosixDirectoryTest, FinalizePublishesCompleteDirectory) {
  ASSERT_NO_FATAL_FAILURE(PrepareData());
  bool renamed = false;
  ASSERT_LEVELDB_OK(FinalizeCheckpointDirectory(env_, parent_, "staging",
                                                "checkpoint", &renamed));
  EXPECT_TRUE(renamed);
  EXPECT_FALSE(env_->FileExists(staging_));
  std::string contents;
  ASSERT_LEVELDB_OK(ReadFileToString(env_, checkpoint_ + "/data", &contents));
  EXPECT_EQ("checkpoint contents", contents);
}

TEST_F(EnvPosixDirectoryTest, ExistingDirectoryIsPreserved) {
  ASSERT_LEVELDB_OK(env_->CreateDir(checkpoint_));
  struct stat before;
  ASSERT_EQ(0, ::stat(checkpoint_.c_str(), &before));
  bool renamed = true;
  EXPECT_TRUE(FinalizeCheckpointDirectory(env_, parent_, "staging",
                                          "checkpoint", &renamed)
                  .IsInvalidArgument());
  EXPECT_FALSE(renamed);
  EXPECT_TRUE(env_->FileExists(staging_));
  struct stat after;
  ASSERT_EQ(0, ::stat(checkpoint_.c_str(), &after));
  EXPECT_EQ(before.st_ino, after.st_ino);
}

TEST_F(EnvPosixDirectoryTest, DanglingSymlinkIsPreserved) {
  ASSERT_EQ(0, ::symlink("missing", checkpoint_.c_str()));
  EXPECT_FALSE(env_->FileExists(checkpoint_));
  bool renamed = true;
  EXPECT_TRUE(FinalizeCheckpointDirectory(env_, parent_, "staging",
                                          "checkpoint", &renamed)
                  .IsInvalidArgument());
  EXPECT_FALSE(renamed);
  EXPECT_TRUE(env_->FileExists(staging_));
  struct stat link_stat;
  ASSERT_EQ(0, ::lstat(checkpoint_.c_str(), &link_stat));
  EXPECT_TRUE(S_ISLNK(link_stat.st_mode));
}

TEST_F(EnvPosixDirectoryTest, ParentSyncFailureLeavesVisibleCheckpoint) {
  class ParentSyncFailureEnv : public EnvWrapper {
   public:
    ParentSyncFailureEnv(Env* target, const std::string& parent)
        : EnvWrapper(target), parent_(parent) {}

    Status SyncDir(const std::string& dir) override {
      if (dir == parent_)
        return Status::IOError("injected parent sync failure");
      return target()->SyncDir(dir);
    }

   private:
    const std::string parent_;
  } failing_env(env_, parent_);

  ASSERT_NO_FATAL_FAILURE(PrepareData());
  bool renamed = false;
  EXPECT_TRUE(FinalizeCheckpointDirectory(&failing_env, parent_, "staging",
                                          "checkpoint", &renamed)
                  .IsIOError());
  EXPECT_TRUE(renamed);
  EXPECT_FALSE(env_->FileExists(staging_));
  std::string contents;
  ASSERT_LEVELDB_OK(ReadFileToString(env_, checkpoint_ + "/data", &contents));
  EXPECT_EQ("checkpoint contents", contents);
}

class EnvPosixCheckpointTest : public EnvPosixDirectoryTest {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(EnvPosixDirectoryTest::SetUp());
    // The creation helper must create and own staging itself.
    ASSERT_LEVELDB_OK(env_->RemoveDir(staging_));
    ASSERT_LEVELDB_OK(WriteStringToFile(env_, "source contents", src_));
  }

  void ExpectSourceUnchanged() {
    std::string contents;
    ASSERT_LEVELDB_OK(ReadFileToString(env_, src_, &contents));
    EXPECT_EQ("source contents", contents);
    struct stat source_stat;
    ASSERT_EQ(0, ::stat(src_.c_str(), &source_stat));
    EXPECT_EQ(1, source_stat.st_nlink);
  }
};

TEST_F(EnvPosixCheckpointTest, CreatePublishesCopiedFiles) {
  bool renamed = false;
  const Status result = CreateCheckpointDirectory(
      env_, parent_, "staging", "checkpoint",
      [&](const std::string& path) {
        return CopyFileContents(env_, src_, path + "/data");
      },
      &renamed);
  ASSERT_LEVELDB_OK(result);
  EXPECT_TRUE(renamed);
  EXPECT_FALSE(env_->FileExists(staging_));
  std::string contents;
  ASSERT_LEVELDB_OK(ReadFileToString(env_, checkpoint_ + "/data", &contents));
  EXPECT_EQ("source contents", contents);
  ExpectSourceUnchanged();
}

TEST_F(EnvPosixCheckpointTest, CopyFailureRemovesStagingButKeepsLinkedSource) {
  bool renamed = true;
  const Status result = CreateCheckpointDirectory(
      env_, parent_, "staging", "checkpoint",
      [&](const std::string& path) {
        Status status = LinkOrCopyFile(env_, src_, path + "/000001.ldb");
        if (!status.ok()) return status;
        // Early EOF removes the partial log. The enclosing operation must
        // also remove the previously linked SSTable and staging directory.
        return CopyFilePrefix(env_, src_, path + "/000002.log", 1000);
      },
      &renamed);
  EXPECT_TRUE(result.IsIOError());
  EXPECT_NE(std::string::npos, result.ToString().find("unexpected EOF"));
  EXPECT_FALSE(renamed);
  EXPECT_FALSE(env_->FileExists(staging_));
  EXPECT_FALSE(env_->FileExists(checkpoint_));
  ExpectSourceUnchanged();
}

TEST_F(EnvPosixCheckpointTest, ExistingStagingAndCheckpointStayUntouched) {
  for (const std::string& existing : {staging_, checkpoint_}) {
    SCOPED_TRACE(existing);
    ASSERT_LEVELDB_OK(env_->CreateDir(existing));
    ASSERT_LEVELDB_OK(WriteStringToFile(env_, "keep", existing + "/data"));
    bool called = false;
    bool renamed = true;
    const Status result = CreateCheckpointDirectory(
        env_, parent_, "staging", "checkpoint",
        [&](const std::string&) {
          called = true;
          return Status::OK();
        },
        &renamed);
    EXPECT_TRUE(result.IsInvalidArgument());
    EXPECT_FALSE(called);
    EXPECT_FALSE(renamed);
    std::string contents;
    ASSERT_LEVELDB_OK(ReadFileToString(env_, existing + "/data", &contents));
    EXPECT_EQ("keep", contents);
    ASSERT_LEVELDB_OK(env_->RemoveFile(existing + "/data"));
    ASSERT_LEVELDB_OK(env_->RemoveDir(existing));
  }
  ExpectSourceUnchanged();
}

TEST_F(EnvPosixCheckpointTest, CleanupUnlinksSymlinksWithoutFollowingThem) {
  const Status error = Status::Corruption("populate failed");
  bool renamed = true;
  const Status result = CreateCheckpointDirectory(
      env_, parent_, "staging", "checkpoint",
      [&](const std::string& path) {
        if (::symlink(src_.c_str(), (path + "/file-link").c_str()) != 0 ||
            ::symlink(parent_.c_str(), (path + "/dir-link").c_str()) != 0 ||
            ::symlink("missing", (path + "/dangling-link").c_str()) != 0) {
          return Status::IOError("symlink setup failed", std::strerror(errno));
        }
        return error;
      },
      &renamed);
  EXPECT_EQ(error.ToString(), result.ToString());
  EXPECT_FALSE(renamed);
  EXPECT_FALSE(env_->FileExists(staging_));
  EXPECT_FALSE(env_->FileExists(checkpoint_));
  EXPECT_TRUE(env_->FileExists(parent_));
  ExpectSourceUnchanged();
}

TEST_F(EnvPosixCheckpointTest, UnexpectedChildDirectoryIsNotTraversed) {
  const Status error = Status::Corruption("populate failed");
  bool renamed = true;
  const std::string child_dir = staging_ + "/unexpected";
  const Status result = CreateCheckpointDirectory(
      env_, parent_, "staging", "checkpoint",
      [&](const std::string&) {
        Status status = env_->CreateDir(child_dir);
        if (!status.ok()) return status;
        status = WriteStringToFile(env_, "keep", child_dir + "/data");
        return status.ok() ? error : status;
      },
      &renamed);
  EXPECT_TRUE(result.IsIOError());
  EXPECT_NE(std::string::npos, result.ToString().find("populate failed"));
  EXPECT_NE(std::string::npos,
            result.ToString().find("cleaning staging directory"));
  EXPECT_FALSE(renamed);
  EXPECT_FALSE(env_->FileExists(checkpoint_));
  std::string contents;
  EXPECT_LEVELDB_OK(ReadFileToString(env_, child_dir + "/data", &contents));
  EXPECT_EQ("keep", contents);
  // Explicit test-fixture cleanup of the deliberately unexpected child.
  EXPECT_LEVELDB_OK(env_->RemoveFile(child_dir + "/data"));
  EXPECT_LEVELDB_OK(env_->RemoveDir(child_dir));
  ExpectSourceUnchanged();
}

TEST_F(EnvPosixCheckpointTest, ErrorsAfterRenameKeepPublishedFiles) {
  class PublishErrorEnv : public EnvWrapper {
   public:
    PublishErrorEnv(Env* target, const std::string& parent, bool rename_error)
        : EnvWrapper(target), parent_(parent), rename_error_(rename_error) {}

    Status RenameFile(const std::string& from, const std::string& to) override {
      const Status status = target()->RenameFile(from, to);
      if (status.ok() && rename_error_) {
        return Status::IOError("ambiguous rename failure");
      }
      return status;
    }

    Status SyncDir(const std::string& dir) override {
      if (dir == parent_) return Status::IOError("parent sync failure");
      return target()->SyncDir(dir);
    }

   private:
    const std::string parent_;
    const bool rename_error_;
  };

  for (bool rename_error : {false, true}) {
    SCOPED_TRACE(rename_error);
    PublishErrorEnv failing_env(env_, parent_, rename_error);
    bool renamed = false;
    const Status result = CreateCheckpointDirectory(
        &failing_env, parent_, "staging", "checkpoint",
        [&](const std::string& path) {
          return CopyFileContents(&failing_env, src_, path + "/data");
        },
        &renamed);
    EXPECT_TRUE(result.IsIOError());
    EXPECT_EQ(!rename_error, renamed);
    EXPECT_FALSE(env_->FileExists(staging_));
    std::string contents;
    ASSERT_LEVELDB_OK(ReadFileToString(env_, checkpoint_ + "/data", &contents));
    EXPECT_EQ("source contents", contents);
    ASSERT_LEVELDB_OK(env_->RemoveFile(checkpoint_ + "/data"));
    ASSERT_LEVELDB_OK(env_->RemoveDir(checkpoint_));
    ExpectSourceUnchanged();
  }
}

class ExecuteCheckpointEnv : public EnvWrapper {
 public:
  enum Failure {
    kNone,
    kLinkUnsupported,
    kAppendUnsupported,
    kLinkError,
    kLinkedSyncError,
    kLinkedCloseError,
    kCurrentAppendError,
    kCurrentSyncError,
    kCurrentCloseError,
    kDirectorySyncUnsupported,
    kStageSyncError,
    kRenameError,
    kParentSyncError
  };

  ExecuteCheckpointEnv(Env* target, const std::string& parent,
                       const std::string& staging, Failure failure = kNone)
      : EnvWrapper(target),
        parent_(parent),
        staging_(staging),
        failure_(failure) {}

  Status LinkFile(const std::string& src, const std::string& dst) override {
    ++link_calls;
    if (failure_ == kLinkUnsupported) {
      return Status::NotSupported("injected link unsupported");
    }
    if (failure_ == kLinkError) {
      return Status::IOError("injected link failure");
    }
    return target()->LinkFile(src, dst);
  }

  Status NewAppendableFile(const std::string& path,
                           WritableFile** result) override {
    *result = nullptr;
    if (failure_ == kAppendUnsupported) {
      return Status::NotSupported("injected append-open unsupported");
    }
    WritableFile* file;
    Status status = target()->NewAppendableFile(path, &file);
    if (status.ok()) *result = new LinkedFile(this, file);
    return status;
  }

  Status NewWritableFile(const std::string& path,
                         WritableFile** result) override {
    Status status = target()->NewWritableFile(path, result);
    if (status.ok() && path == staging_ + "/CURRENT") {
      *result = new CurrentFile(this, *result);
    }
    return status;
  }

  Status CreateDir(const std::string& path) override {
    ++create_dir_calls;
    return target()->CreateDir(path);
  }

  Status SyncDir(const std::string& path) override {
    if (failure_ == kDirectorySyncUnsupported) {
      return Status::NotSupported("injected directory sync unsupported");
    }
    if ((failure_ == kStageSyncError && path == staging_) ||
        (failure_ == kParentSyncError && renamed_ && path == parent_)) {
      return Status::IOError("injected directory sync failure");
    }
    return target()->SyncDir(path);
  }

  Status RenameFile(const std::string& src, const std::string& dst) override {
    if (failure_ == kRenameError) {
      return Status::IOError("injected rename failure");
    }
    Status status = target()->RenameFile(src, dst);
    if (status.ok()) renamed_ = true;
    return status;
  }

  int link_calls = 0;
  int linked_syncs = 0;
  int linked_closes = 0;
  int current_closes = 0;
  int create_dir_calls = 0;

 private:
  // A linked inode may be synced through an append handle, but no bytes may
  // be appended because that would also modify the original DB file.
  class LinkedFile : public WritableFile {
   public:
    LinkedFile(ExecuteCheckpointEnv* env, WritableFile* file)
        : env_(env), file_(file) {}
    ~LinkedFile() override { delete file_; }

    Status Append(const Slice&) override {
      ADD_FAILURE() << "ExecuteCheckpoint appended to a linked file";
      return Status::IOError("unexpected append to linked file");
    }
    Status Close() override {
      ++env_->linked_closes;
      const Status status = file_->Close();
      if (status.ok() && env_->failure_ == kLinkedCloseError) {
        return Status::IOError("injected linked file close failure");
      }
      return status;
    }
    Status Flush() override { return file_->Flush(); }
    Status Sync() override {
      ++env_->linked_syncs;
      if (env_->failure_ == kLinkedSyncError) {
        return Status::IOError("injected linked file sync failure");
      }
      return file_->Sync();
    }

   private:
    ExecuteCheckpointEnv* const env_;
    WritableFile* const file_;
  };

  class CurrentFile : public WritableFile {
   public:
    CurrentFile(ExecuteCheckpointEnv* env, WritableFile* file)
        : env_(env), file_(file) {}
    ~CurrentFile() override { delete file_; }

    Status Append(const Slice& data) override {
      if (env_->failure_ == kCurrentAppendError) {
        return Status::IOError("injected CURRENT append failure");
      }
      return file_->Append(data);
    }
    Status Close() override {
      ++env_->current_closes;
      const Status status = file_->Close();
      if (status.ok() && env_->failure_ == kCurrentCloseError) {
        return Status::IOError("injected CURRENT close failure");
      }
      return status;
    }
    Status Flush() override { return file_->Flush(); }
    Status Sync() override {
      if (env_->failure_ == kCurrentSyncError) {
        return Status::IOError("injected CURRENT sync failure");
      }
      return file_->Sync();
    }

   private:
    ExecuteCheckpointEnv* const env_;
    WritableFile* const file_;
  };

  const std::string parent_;
  const std::string staging_;
  const Failure failure_;
  bool renamed_ = false;
};

class EnvPosixExecuteCheckpointTest : public EnvPosixDirectoryTest {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(EnvPosixDirectoryTest::SetUp());
    ASSERT_LEVELDB_OK(env_->RemoveDir(staging_));
    staging_ = checkpoint_ + ".tmp";
    plan_.link_files = {"000001.ldb", "000002.log"};
    plan_.live_log_file = "000003.log";
    plan_.live_log_size = 4;
    plan_.manifest_file = "MANIFEST-000004";
    plan_.manifest_size = 8;
    ASSERT_LEVELDB_OK(
        WriteStringToFile(env_, "table contents", parent_ + "/000001.ldb"));
    ASSERT_LEVELDB_OK(
        WriteStringToFile(env_, "closed log", parent_ + "/000002.log"));
    ASSERT_LEVELDB_OK(
        WriteStringToFile(env_, "live-tail", parent_ + "/000003.log"));
    ASSERT_LEVELDB_OK(
        WriteStringToFile(env_, "manifest-tail", parent_ + "/MANIFEST-000004"));
  }

  void ExpectContents(const std::string& path, const std::string& expected) {
    std::string contents;
    ASSERT_LEVELDB_OK(ReadFileToString(env_, path, &contents));
    EXPECT_EQ(expected, contents) << path;
  }

  void ExpectSourcesUnchanged() {
    ExpectContents(parent_ + "/000001.ldb", "table contents");
    ExpectContents(parent_ + "/000002.log", "closed log");
    ExpectContents(parent_ + "/000003.log", "live-tail");
    ExpectContents(parent_ + "/MANIFEST-000004", "manifest-tail");
  }

  void ExpectCompleteCheckpoint(bool linked) {
    EXPECT_FALSE(env_->FileExists(staging_));
    ExpectContents(checkpoint_ + "/000001.ldb", "table contents");
    ExpectContents(checkpoint_ + "/000002.log", "closed log");
    ExpectContents(checkpoint_ + "/000003.log", "live");
    ExpectContents(checkpoint_ + "/MANIFEST-000004", "manifest");
    ExpectContents(checkpoint_ + "/CURRENT", "MANIFEST-000004\n");
    for (const char* name :
         {"000001.ldb", "000002.log", "000003.log", "MANIFEST-000004"}) {
      struct stat source_stat;
      struct stat copy_stat;
      ASSERT_EQ(0, ::stat((parent_ + "/" + name).c_str(), &source_stat));
      ASSERT_EQ(0, ::stat((checkpoint_ + "/" + name).c_str(), &copy_stat));
      const bool immutable = std::string(name) == "000001.ldb" ||
                             std::string(name) == "000002.log";
      EXPECT_EQ(linked && immutable, source_stat.st_ino == copy_stat.st_ino);
    }
    ExpectSourcesUnchanged();
  }

  void ExpectNoCheckpoint() {
    EXPECT_FALSE(env_->FileExists(staging_));
    EXPECT_FALSE(env_->FileExists(checkpoint_));
    for (const std::string& name : plan_.link_files) {
      struct stat source_stat;
      ASSERT_EQ(0, ::stat((parent_ + "/" + name).c_str(), &source_stat));
      EXPECT_EQ(1, source_stat.st_nlink);
    }
    ExpectSourcesUnchanged();
  }

  CheckpointPlan plan_;
};

TEST_F(EnvPosixExecuteCheckpointTest, LinksSyncsAndCopiesSelectedPrefixes) {
  ExecuteCheckpointEnv checked(env_, parent_, staging_);
  ASSERT_LEVELDB_OK(ExecuteCheckpoint(&checked, plan_, parent_, checkpoint_));
  EXPECT_EQ(2, checked.link_calls);
  EXPECT_EQ(2, checked.linked_syncs);
  EXPECT_EQ(2, checked.linked_closes);
  EXPECT_EQ(1, checked.current_closes);
  ExpectCompleteCheckpoint(true);
}

TEST_F(EnvPosixExecuteCheckpointTest, UnsupportedLinksAreCopied) {
  ExecuteCheckpointEnv checked(env_, parent_, staging_,
                               ExecuteCheckpointEnv::kLinkUnsupported);
  ASSERT_LEVELDB_OK(ExecuteCheckpoint(&checked, plan_, parent_, checkpoint_));
  EXPECT_EQ(2, checked.link_calls);
  EXPECT_EQ(0, checked.linked_syncs);
  ExpectCompleteCheckpoint(false);
}

TEST_F(EnvPosixExecuteCheckpointTest, UnsupportedAppendOpenUnlinksBeforeCopy) {
  ExecuteCheckpointEnv checked(env_, parent_, staging_,
                               ExecuteCheckpointEnv::kAppendUnsupported);
  ASSERT_LEVELDB_OK(ExecuteCheckpoint(&checked, plan_, parent_, checkpoint_));
  EXPECT_EQ(2, checked.link_calls);
  // Copying over a still-linked destination would truncate the source inode.
  ExpectCompleteCheckpoint(false);
}

TEST_F(EnvPosixExecuteCheckpointTest, InvalidPlansCannotOverwriteSources) {
  std::vector<CheckpointPlan> invalid;
  for (const std::string& name :
       {std::string("../000001.ldb"), std::string("CURRENT"),
        std::string("000001.ldb\0suffix", 17), std::string("MANIFEST-000004"),
        std::string("000001.ldb/child")}) {
    CheckpointPlan plan = plan_;
    plan.link_files.push_back(name);
    invalid.push_back(plan);
  }
  CheckpointPlan duplicate = plan_;
  duplicate.link_files.push_back(plan_.link_files[0]);
  invalid.push_back(duplicate);
  duplicate = plan_;
  duplicate.link_files.push_back(plan_.live_log_file);
  invalid.push_back(duplicate);
  CheckpointPlan wrong_type = plan_;
  wrong_type.live_log_file = "000001.ldb";
  invalid.push_back(wrong_type);
  wrong_type = plan_;
  wrong_type.manifest_file = "MANIFEST-000004\nother";
  invalid.push_back(wrong_type);
  for (size_t i = 0; i < invalid.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_TRUE(ExecuteCheckpoint(env_, invalid[i], parent_, checkpoint_)
                    .IsInvalidArgument());
    ExpectNoCheckpoint();
  }
}

TEST_F(EnvPosixExecuteCheckpointTest, ExistingPathsArePreserved) {
  for (const std::string& existing : {staging_, checkpoint_}) {
    SCOPED_TRACE(existing);
    ASSERT_LEVELDB_OK(env_->CreateDir(existing));
    ASSERT_LEVELDB_OK(WriteStringToFile(env_, "keep", existing + "/marker"));
    EXPECT_TRUE(ExecuteCheckpoint(env_, plan_, parent_, checkpoint_)
                    .IsInvalidArgument());
    ExpectContents(existing + "/marker", "keep");
    ASSERT_LEVELDB_OK(env_->RemoveFile(existing + "/marker"));
    ASSERT_LEVELDB_OK(env_->RemoveDir(existing));
    ExpectNoCheckpoint();
  }
  ASSERT_EQ(0, ::symlink("missing", checkpoint_.c_str()));
  EXPECT_TRUE(
      ExecuteCheckpoint(env_, plan_, parent_, checkpoint_).IsInvalidArgument());
  struct stat link_stat;
  ASSERT_EQ(0, ::lstat(checkpoint_.c_str(), &link_stat));
  EXPECT_TRUE(S_ISLNK(link_stat.st_mode));
  ASSERT_LEVELDB_OK(env_->RemoveFile(checkpoint_));
  ExpectNoCheckpoint();
}

TEST_F(EnvPosixExecuteCheckpointTest, MissingAndShortPrefixesCleanStaging) {
  for (int failure = 0; failure < 3; ++failure) {
    SCOPED_TRACE(failure);
    CheckpointPlan plan = plan_;
    if (failure == 0) plan.live_log_file = "000099.log";
    if (failure == 1) plan.live_log_size = 1000;
    if (failure == 2) plan.manifest_size = 1000;
    const Status status = ExecuteCheckpoint(env_, plan, parent_, checkpoint_);
    if (failure == 0) {
      EXPECT_TRUE(status.IsNotFound()) << status.ToString();
    } else {
      EXPECT_TRUE(status.IsIOError()) << status.ToString();
      EXPECT_NE(std::string::npos, status.ToString().find("unexpected EOF"));
    }
    ExpectNoCheckpoint();
  }
}

TEST_F(EnvPosixExecuteCheckpointTest, FailuresBeforeRenameCleanStaging) {
  for (ExecuteCheckpointEnv::Failure failure :
       {ExecuteCheckpointEnv::kLinkError,
        ExecuteCheckpointEnv::kLinkedSyncError,
        ExecuteCheckpointEnv::kLinkedCloseError,
        ExecuteCheckpointEnv::kCurrentAppendError,
        ExecuteCheckpointEnv::kCurrentSyncError,
        ExecuteCheckpointEnv::kCurrentCloseError,
        ExecuteCheckpointEnv::kStageSyncError,
        ExecuteCheckpointEnv::kRenameError}) {
    SCOPED_TRACE(failure);
    ExecuteCheckpointEnv checked(env_, parent_, staging_, failure);
    const Status status =
        ExecuteCheckpoint(&checked, plan_, parent_, checkpoint_);
    EXPECT_TRUE(status.IsIOError()) << status.ToString();
    if (failure == ExecuteCheckpointEnv::kLinkError) {
      EXPECT_EQ(1, checked.link_calls);
      EXPECT_NE(std::string::npos, status.ToString().find("link failure"));
    }
    if (failure == ExecuteCheckpointEnv::kLinkedSyncError ||
        failure == ExecuteCheckpointEnv::kLinkedCloseError) {
      EXPECT_EQ(1, checked.linked_syncs);
      EXPECT_EQ(1, checked.linked_closes);
    }
    if (failure == ExecuteCheckpointEnv::kCurrentAppendError ||
        failure == ExecuteCheckpointEnv::kCurrentSyncError ||
        failure == ExecuteCheckpointEnv::kCurrentCloseError) {
      EXPECT_EQ(1, checked.current_closes);
      EXPECT_NE(std::string::npos, status.ToString().find("CURRENT"));
    }
    ExpectNoCheckpoint();
  }
}

TEST_F(EnvPosixExecuteCheckpointTest, UnsupportedDirectorySyncCreatesNothing) {
  ExecuteCheckpointEnv checked(env_, parent_, staging_,
                               ExecuteCheckpointEnv::kDirectorySyncUnsupported);
  EXPECT_TRUE(ExecuteCheckpoint(&checked, plan_, parent_, checkpoint_)
                  .IsNotSupportedError());
  EXPECT_EQ(0, checked.create_dir_calls);
  EXPECT_EQ(0, checked.link_calls);
  ExpectNoCheckpoint();
}

TEST_F(EnvPosixExecuteCheckpointTest, ParentSyncFailureKeepsPublishedFiles) {
  ExecuteCheckpointEnv checked(env_, parent_, staging_,
                               ExecuteCheckpointEnv::kParentSyncError);
  EXPECT_TRUE(
      ExecuteCheckpoint(&checked, plan_, parent_, checkpoint_).IsIOError());
  ExpectCompleteCheckpoint(true);
}

TEST_F(EnvPosixTest, TestOpenOnRead) {
  // Write some test data to a single file that will be opened |n| times.
  std::string test_dir;
  ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
  std::string test_file = test_dir + "/open_on_read.txt";

  FILE* f = std::fopen(test_file.c_str(), "we");
  ASSERT_TRUE(f != nullptr);
  const char kFileData[] = "abcdefghijklmnopqrstuvwxyz";
  fputs(kFileData, f);
  std::fclose(f);

  // Open test file some number above the sum of the two limits to force
  // open-on-read behavior of POSIX Env leveldb::RandomAccessFile.
  const int kNumFiles = kReadOnlyFileLimit + kMMapLimit + 5;
  leveldb::RandomAccessFile* files[kNumFiles] = {0};
  for (int i = 0; i < kNumFiles; i++) {
    ASSERT_LEVELDB_OK(env_->NewRandomAccessFile(test_file, &files[i]));
  }
  char scratch;
  Slice read_result;
  for (int i = 0; i < kNumFiles; i++) {
    ASSERT_LEVELDB_OK(files[i]->Read(i, 1, &read_result, &scratch));
    ASSERT_EQ(kFileData[i], read_result[0]);
  }
  for (int i = 0; i < kNumFiles; i++) {
    delete files[i];
  }
  ASSERT_LEVELDB_OK(env_->RemoveFile(test_file));
}

#if HAVE_O_CLOEXEC

TEST_F(EnvPosixTest, TestCloseOnExecSequentialFile) {
  std::unordered_set<int> open_fds;
  GetOpenFileDescriptors(&open_fds);

  std::string test_dir;
  ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
  std::string file_path = test_dir + "/close_on_exec_sequential.txt";
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "0123456789", file_path));

  leveldb::SequentialFile* file = nullptr;
  ASSERT_LEVELDB_OK(env_->NewSequentialFile(file_path, &file));
  CheckCloseOnExecDoesNotLeakFDs(open_fds);
  delete file;

  ASSERT_LEVELDB_OK(env_->RemoveFile(file_path));
}

TEST_F(EnvPosixTest, TestCloseOnExecRandomAccessFile) {
  std::unordered_set<int> open_fds;
  GetOpenFileDescriptors(&open_fds);

  std::string test_dir;
  ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
  std::string file_path = test_dir + "/close_on_exec_random_access.txt";
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "0123456789", file_path));

  // Exhaust the RandomAccessFile mmap limit. This way, the test
  // RandomAccessFile instance below is backed by a file descriptor, not by an
  // mmap region.
  leveldb::RandomAccessFile* mmapped_files[kMMapLimit];
  for (int i = 0; i < kMMapLimit; i++) {
    ASSERT_LEVELDB_OK(env_->NewRandomAccessFile(file_path, &mmapped_files[i]));
  }

  leveldb::RandomAccessFile* file = nullptr;
  ASSERT_LEVELDB_OK(env_->NewRandomAccessFile(file_path, &file));
  CheckCloseOnExecDoesNotLeakFDs(open_fds);
  delete file;

  for (int i = 0; i < kMMapLimit; i++) {
    delete mmapped_files[i];
  }
  ASSERT_LEVELDB_OK(env_->RemoveFile(file_path));
}

TEST_F(EnvPosixTest, TestCloseOnExecWritableFile) {
  std::unordered_set<int> open_fds;
  GetOpenFileDescriptors(&open_fds);

  std::string test_dir;
  ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
  std::string file_path = test_dir + "/close_on_exec_writable.txt";
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "0123456789", file_path));

  leveldb::WritableFile* file = nullptr;
  ASSERT_LEVELDB_OK(env_->NewWritableFile(file_path, &file));
  CheckCloseOnExecDoesNotLeakFDs(open_fds);
  delete file;

  ASSERT_LEVELDB_OK(env_->RemoveFile(file_path));
}

TEST_F(EnvPosixTest, TestCloseOnExecAppendableFile) {
  std::unordered_set<int> open_fds;
  GetOpenFileDescriptors(&open_fds);

  std::string test_dir;
  ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
  std::string file_path = test_dir + "/close_on_exec_appendable.txt";
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "0123456789", file_path));

  leveldb::WritableFile* file = nullptr;
  ASSERT_LEVELDB_OK(env_->NewAppendableFile(file_path, &file));
  CheckCloseOnExecDoesNotLeakFDs(open_fds);
  delete file;

  ASSERT_LEVELDB_OK(env_->RemoveFile(file_path));
}

TEST_F(EnvPosixTest, TestCloseOnExecLockFile) {
  std::unordered_set<int> open_fds;
  GetOpenFileDescriptors(&open_fds);

  std::string test_dir;
  ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
  std::string file_path = test_dir + "/close_on_exec_lock.txt";
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "0123456789", file_path));

  leveldb::FileLock* lock = nullptr;
  ASSERT_LEVELDB_OK(env_->LockFile(file_path, &lock));
  CheckCloseOnExecDoesNotLeakFDs(open_fds);
  ASSERT_LEVELDB_OK(env_->UnlockFile(lock));

  ASSERT_LEVELDB_OK(env_->RemoveFile(file_path));
}

TEST_F(EnvPosixTest, TestCloseOnExecLogger) {
  std::unordered_set<int> open_fds;
  GetOpenFileDescriptors(&open_fds);

  std::string test_dir;
  ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
  std::string file_path = test_dir + "/close_on_exec_logger.txt";
  ASSERT_LEVELDB_OK(WriteStringToFile(env_, "0123456789", file_path));

  leveldb::Logger* file = nullptr;
  ASSERT_LEVELDB_OK(env_->NewLogger(file_path, &file));
  CheckCloseOnExecDoesNotLeakFDs(open_fds);
  delete file;

  ASSERT_LEVELDB_OK(env_->RemoveFile(file_path));
}

#endif  // HAVE_O_CLOEXEC

}  // namespace leveldb

int main(int argc, char** argv) {
#if HAVE_O_CLOEXEC
  // Check if we're invoked as a helper program, or as the test suite.
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], kTestCloseOnExecSwitch)) {
      return TestCloseOnExecHelperMain(argv[i + 1]);
    }
  }

  // Save argv[0] early, because googletest may modify argv.
  GetArgvZero()->assign(argv[0], argv[0] + std::strlen(argv[0]) + 1);
#endif  // HAVE_O_CLOEXEC

  // All tests currently run with the same read-only file limits.
  leveldb::EnvPosixTest::SetFileLimits(leveldb::kReadOnlyFileLimit,
                                       leveldb::kMMapLimit);

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
