// Copyright 2024 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <poll.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "gtest/gtest.h"
#include "test/util/test_util.h"
#include "test/util/thread_util.h"
#include "test/util/time_util.h"


namespace gvisor {
namespace testing {

class PidfdTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {
    while (waitpid(-1, nullptr, 0) != -1) {}
  }
};

// Creates a child process, gets its pidfd, waits for it to exit,
// and return the now-invalid pidfd
int CreateAndWaitForDeadPidfd() {
  pid_t child_pid = fork();
  if (child_pid == 0) {
    _exit(0);
  }
  
  int pidfd = syscall(SYS_pidfd_open, child_pid, 0);
  EXPECT_THAT(pidfd, SyscallSucceeds());
  
  EXPECT_THAT(waitpid(child_pid, nullptr, 0), SyscallSucceedsWithValue(child_pid));
  return pidfd;
}

TEST_F(PidfdTest, Basic) {
  pid_t pid = getpid();
  EXPECT_THAT(syscall(SYS_pidfd_open, pid, 0), SyscallSucceeds());
}

TEST_F(PidfdTest, InvalidFlags) {
  pid_t pid = getpid();
  EXPECT_THAT(syscall(SYS_pidfd_open, pid, 0x4), SyscallFailsWithErrno(EINVAL));
  EXPECT_THAT(syscall(SYS_pidfd_open, pid, ~0u), SyscallFailsWithErrno(EINVAL));
}

TEST_F(PidfdTest, InvalidPid) {
  EXPECT_THAT(syscall(SYS_pidfd_open, -1, 0), SyscallFailsWithErrno(EINVAL));
  EXPECT_THAT(syscall(SYS_pidfd_open, 0, 0), SyscallFailsWithErrno(EINVAL));
  EXPECT_THAT(syscall(SYS_pidfd_open, 999999, 0), SyscallFailsWithErrno(ESRCH));
}


TEST_F(PidfdTest, ValidFlags) {
  pid_t pid = getpid();
  EXPECT_THAT(syscall(SYS_pidfd_open, pid, O_NONBLOCK), SyscallSucceeds());
}

TEST_F(PidfdTest, PollWaitForProcessExit) {
  pid_t child_pid = fork();
  if (child_pid == 0) {
    SleepSafe(absl::Seconds(1));
    _exit(0);
  }

  int pidfd = syscall(SYS_pidfd_open, child_pid, 0);
  ASSERT_THAT(pidfd, SyscallSucceeds());

  struct pollfd pfd;
  pfd.fd = pidfd;
  pfd.events = POLLIN;

  EXPECT_THAT(poll(&pfd, 1, -1), SyscallSucceedsWithValue(1));
  EXPECT_EQ(pfd.revents & POLLIN, POLLIN);

  EXPECT_THAT(close(pidfd), SyscallSucceeds());
}


TEST_F(PidfdTest, PollNonblocking) {
  pid_t child_pid = fork();
  if (child_pid == 0) {
    SleepSafe(absl::Seconds(2));
    _exit(0);
  }

  int pidfd = syscall(SYS_pidfd_open, child_pid, 0);
  ASSERT_THAT(pidfd, SyscallSucceeds());

  struct pollfd pfd;
  pfd.fd = pidfd;
  pfd.events = POLLIN;

  EXPECT_THAT(poll(&pfd, 1, 0), SyscallSucceedsWithValue(0));
  EXPECT_EQ(pfd.revents, 0);

  pfd.events = POLLOUT;
  EXPECT_THAT(poll(&pfd, 1, 0), SyscallSucceedsWithValue(0));
  EXPECT_EQ(pfd.revents, 0);

  ASSERT_THAT(waitpid(child_pid, nullptr, 0), SyscallSucceedsWithValue(child_pid));
  EXPECT_THAT(close(pidfd), SyscallSucceeds());
}

TEST_F(PidfdTest, PollInvalidPidfd) {
  int dead_pidfd = CreateAndWaitForDeadPidfd();
  
  struct pollfd pfd;
  pfd.fd = dead_pidfd;
  pfd.events = POLLIN;
  
  EXPECT_THAT(poll(&pfd, 1, 0), SyscallSucceedsWithValue(1));
  EXPECT_EQ(pfd.revents & POLLIN, POLLIN);
  
  EXPECT_THAT(close(dead_pidfd), SyscallSucceeds());
}

TEST_F(PidfdTest, Select) {
  pid_t child_pid = fork();
  if (child_pid == 0) {
    SleepSafe(absl::Seconds(1));
    _exit(0);
  }

  int pidfd = syscall(SYS_pidfd_open, child_pid, 0);
  ASSERT_THAT(pidfd, SyscallSucceeds());

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(pidfd, &readfds);

  // Select while process is still running
  struct timeval timeout = {0, 0};
  EXPECT_THAT(select(pidfd + 1, &readfds, nullptr, nullptr, &timeout),
              SyscallSucceedsWithValue(0));
  EXPECT_FALSE(FD_ISSET(pidfd, &readfds));

  // Wait for child to exit
  ASSERT_THAT(waitpid(child_pid, nullptr, 0), SyscallSucceedsWithValue(child_pid));

  // Select after child has exited
  FD_SET(pidfd, &readfds);
  EXPECT_THAT(select(pidfd + 1, &readfds, nullptr, nullptr, nullptr),
              SyscallSucceedsWithValue(1));
  EXPECT_TRUE(FD_ISSET(pidfd, &readfds));

  EXPECT_THAT(close(pidfd), SyscallSucceeds());
}

TEST_F(PidfdTest, Epoll) {
  pid_t child_pid = fork();
  if (child_pid == 0) {
    SleepSafe(absl::Seconds(1));
    _exit(0);
  }

  int pidfd = syscall(SYS_pidfd_open, child_pid, 0);
  ASSERT_THAT(pidfd, SyscallSucceeds());

  int epfd = epoll_create1(0);
  ASSERT_THAT(epfd, SyscallSucceeds());

  struct epoll_event event;
  event.events = EPOLLIN;
  event.data.fd = pidfd;
  ASSERT_THAT(epoll_ctl(epfd, EPOLL_CTL_ADD, pidfd, &event), SyscallSucceeds());

  // Epoll while process is still running
  struct epoll_event events[1];
  EXPECT_THAT(epoll_wait(epfd, events, 1, 0), SyscallSucceedsWithValue(0));

  // Wait for child to exit
  ASSERT_THAT(waitpid(child_pid, nullptr, 0), SyscallSucceedsWithValue(child_pid));

  // Epoll after child has exited
  EXPECT_THAT(epoll_wait(epfd, events, 1, -1), SyscallSucceedsWithValue(1));
  EXPECT_EQ(events[0].data.fd, pidfd);
  EXPECT_EQ(events[0].events & EPOLLIN, EPOLLIN);

  EXPECT_THAT(close(epfd), SyscallSucceeds());
  EXPECT_THAT(close(pidfd), SyscallSucceeds());
}

TEST_F(PidfdTest, EpollOneShot) {
  pid_t child_pid = fork();
  if (child_pid == 0) {
    SleepSafe(absl::Seconds(2));
    _exit(0);
  }

  int pidfd = syscall(SYS_pidfd_open, child_pid, 0);
  ASSERT_THAT(pidfd, SyscallSucceeds());

  int epfd = epoll_create1(0);
  ASSERT_THAT(epfd, SyscallSucceeds());

  // Add pidfd to epoll with EPOLLONESHOT
  struct epoll_event event;
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.fd = pidfd;
  ASSERT_THAT(epoll_ctl(epfd, EPOLL_CTL_ADD, pidfd, &event), SyscallSucceeds());

  // Wait for child to exit
  ASSERT_THAT(waitpid(child_pid, nullptr, 0), SyscallSucceedsWithValue(child_pid));

  // First epoll_wait should succeed and get the event
  struct epoll_event events[1];
  EXPECT_THAT(epoll_wait(epfd, events, 1, -1), SyscallSucceedsWithValue(1));
  EXPECT_EQ(events[0].data.fd, pidfd);
  EXPECT_EQ(events[0].events & EPOLLIN, EPOLLIN);

  // Second epoll_wait should timeout because EPOLLONESHOT disabled the fd
  EXPECT_THAT(epoll_wait(epfd, events, 1, 100), SyscallSucceedsWithValue(0));

  // Rearm with MOD
  EXPECT_THAT(epoll_ctl(epfd, EPOLL_CTL_MOD, pidfd, &event), SyscallSucceeds());

  // Now we should get the event again
  EXPECT_THAT(epoll_wait(epfd, events, 1, -1), SyscallSucceedsWithValue(1));
  
  EXPECT_THAT(close(epfd), SyscallSucceeds());
  EXPECT_THAT(close(pidfd), SyscallSucceeds());
}

// "read(2) on the file descriptor fails with the error EINVAL"

TEST_F(PidfdTest, Read) {
  pid_t pid = getpid();
  int pidfd = syscall(SYS_pidfd_open, pid, 0);
  ASSERT_THAT(pidfd, SyscallSucceeds());

  char buf[1];
  EXPECT_THAT(read(pidfd, buf, sizeof(buf)), SyscallFailsWithErrno(EINVAL));

  EXPECT_THAT(close(pidfd), SyscallSucceeds());
}


// "If the process referred to by the file descriptor has not yet
// terminated, then an attempt to wait on the file descriptor using
// waitid(2) will immediately return the error EAGAIN rather than
// blocking."

TEST_F(PidfdTest, WaitNonblockingBehavior) {
  pid_t child_pid = fork();
  if (child_pid == 0) {
    // sleep briefly to ensure parent can set up wait
    SleepSafe(absl::Seconds(1));
    _exit(0);
  }

  // open child's pidfd with O_NONBLOCK
  int pidfd = syscall(SYS_pidfd_open, child_pid, O_NONBLOCK);
  ASSERT_THAT(pidfd, SyscallSucceeds());

  siginfo_t info;
  EXPECT_THAT(waitid(P_PIDFD, pidfd, &info, WEXITED), 
              SyscallFailsWithErrno(EAGAIN));

  EXPECT_THAT(close(pidfd), SyscallSucceeds());
  ASSERT_THAT(waitpid(child_pid, nullptr, 0), SyscallSucceedsWithValue(child_pid));
}

TEST_F(PidfdTest, WaitBlockingBehavior) {
  pid_t pid = getpid();
  int pidfd = syscall(SYS_pidfd_open, pid, 0);
  ASSERT_THAT(pidfd, SyscallSucceeds());

  siginfo_t info;
  pid_t child_pid = fork();
  if (child_pid == 0) {
    _exit(0);
  }

  int child_pidfd = syscall(SYS_pidfd_open, child_pid, 0);
  ASSERT_THAT(child_pidfd, SyscallSucceeds());

  EXPECT_THAT(waitid(P_PIDFD, child_pidfd, &info, WEXITED), 
              SyscallSucceeds());

  EXPECT_THAT(close(child_pidfd), SyscallSucceeds());
  EXPECT_THAT(close(pidfd), SyscallSucceeds());
}

TEST_F(PidfdTest, WaitInvalidPidfd) {
  int dead_pidfd = CreateAndWaitForDeadPidfd();
  
  siginfo_t info;
  EXPECT_THAT(waitid(P_PIDFD, dead_pidfd, &info, WEXITED),
              SyscallFailsWithErrno(ECHILD));
  EXPECT_THAT(waitid(P_PIDFD, -1, &info, WEXITED),
              SyscallFailsWithErrno(EINVAL));
}

}  // namespace testing
}  // namespace gvisor
