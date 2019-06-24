//
// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "absl/debugging/failure_signal_handler.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <iostream>

#include "gtest/gtest.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/debugging/stacktrace.h"
#include "absl/debugging/symbolize.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

namespace {

#if GTEST_HAS_DEATH_TEST

// For the parameterized death tests. GetParam() returns the signal number.
using FailureSignalHandlerDeathTest = ::testing::TestWithParam<int>;

// This function runs in a fork()ed process on most systems.
void InstallHandlerAndRaise(int signo) {
  absl::InstallFailureSignalHandler(absl::FailureSignalHandlerOptions());
  raise(signo);
}

#define READ_BUFFER_SIZE 1024

// After forking, parent kills child. Directly taken from:
// https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/platform/stacktrace_handler_test.cc#L32
// ABSL stacktrace is: 
// [ RUN      ] StacktraceHandlerTest.GeneratesStacktraceFails
// Output from the child process:*** SIGABRT received at time=1561409570 ***
// PC: @     0x7faef9c1cff4  (unknown)  (unknown)
//     @     0x7faefac72807         64  absl::WriteFailureInfo()
//     @     0x7faefac729a9         96  absl::AbslFailureSignalHandler()
//     @     0x7faef9f283a0  (unknown)  (unknown)
// absl/debugging/failure_signal_handler_test.cc:99: Failure
// Expected: (child_output.find(test_stack_frame)) != (std::string::npos), actual: 18446744073709551615 vs 18446744073709551615
TEST(StacktraceHandlerTest, GeneratesStacktraceFails) {
  absl::InstallFailureSignalHandler(absl::FailureSignalHandlerOptions());
  // Create a pipe to write/read the child stdout.
  int test_pipe[2];
  EXPECT_EQ(pipe(test_pipe), 0);

  // Fork the process.
  int test_pid = fork();

  if (test_pid == 0) {
    // Child process.
    // Close the read end of the pipe, redirect stdout and sleep.
    close(test_pipe[0]);
    dup2(test_pipe[1], STDOUT_FILENO);
    dup2(test_pipe[1], STDERR_FILENO);
    sleep(10);
  } else {
    // Parent process.
    // Close the write end of the pipe, wait a little and send SIGABRT to the
    // child process. Then watch the pipe.
    close(test_pipe[1]);
    sleep(1);

    // Send the signal.
    kill(test_pid, SIGABRT);

    // Read from the pipe.
    char buffer[READ_BUFFER_SIZE];
    std::string child_output = "";
    while (true) {
      int read_length = read(test_pipe[0], buffer, READ_BUFFER_SIZE);
      if (read_length > 0) {
        child_output += std::string(buffer, read_length);
      } else {
        break;
      }
    }
    close(test_pipe[0]);

    // Just make sure we can detect one of the calls in testing stack.
    std::string test_stack_frame = "testing::internal::UnitTestImpl::RunAllTests()";

    // Print the stack trace detected for information.
    std::cerr << "Output from the child process:";
    std::cerr << child_output;

    EXPECT_NE(child_output.find(test_stack_frame), std::string::npos);
  }
}

// After forking, child calls raise, parent waits on child.
// ABSL stacktrace is: 
// [ RUN      ] StacktraceHandlerTest.GeneratesStacktrace
// Output from the child process:*** SIGABRT received at time=1561409570 ***
// PC: @     0x7faef9f2823b  (unknown)  raise
//     @     0x7faefac72807         64  absl::WriteFailureInfo()
//     @     0x7faefac729a9         96  absl::AbslFailureSignalHandler()
//     @     0x7faef9f283a0  (unknown)  (unknown)
//     @     0x7faefaba1311         48  testing::internal::HandleSehExceptionsInMethodIfSupported<>()
//     @     0x7faefab9cb0f        144  testing::internal::HandleExceptionsInMethodIfSupported<>()
//     @     0x7faefab8781e        112  testing::Test::Run()
//     @     0x7faefab8816f        112  testing::TestInfo::Run()
//     @     0x7faefab887ff        112  testing::TestSuite::Run()
//     @     0x7faefab939cd         96  testing::internal::UnitTestImpl::RunAllTests()
//     @     0x7faefaba238f         48  testing::internal::HandleSehExceptionsInMethodIfSupported<>()
//     @     0x7faefab9d9b3        144  testing::internal::HandleExceptionsInMethodIfSupported<>()
//     @     0x7faefab924dd        128  testing::UnitTest::Run()
//     @     0x55c3ddb5d93c         16  RUN_ALL_TESTS()
//     @     0x55c3ddb5c672         32  main
//     @     0x7faef9b7a52b  (unknown)  (unknown)
//     @ 0x41fd89415541f689  (unknown)  (unknown)
// [       OK ] StacktraceHandlerTest.GeneratesStacktrace (10 ms)
TEST(StacktraceHandlerTest, GeneratesStacktrace) {
  absl::InstallFailureSignalHandler(absl::FailureSignalHandlerOptions());
  // Create a pipe to write/read the child stdout.
  int test_pipe[2];
  EXPECT_EQ(pipe(test_pipe), 0);

  // Fork the process.
  int test_pid = fork();
  EXPECT_NE(test_pid, -1);
  if (test_pid == 0) {
    // Child process.
    // Close the read end of the pipe, redirect stdout and sleep.
    close(test_pipe[0]);
    dup2(test_pipe[1], STDOUT_FILENO);
    dup2(test_pipe[1], STDERR_FILENO);
    raise(SIGABRT);
  } else {
    // Parent process.
    // Close the write end of the pipe, wait a little and send SIGABRT to the
    // child process. Then watch the pipe.
    EXPECT_NE(close(test_pipe[1]), -1);

    int status;
    wait(&status);

    // Read from the pipe.
    char buffer[READ_BUFFER_SIZE];
    std::string child_output = "";
    while (true) {
      int read_length = read(test_pipe[0], buffer, READ_BUFFER_SIZE);
      if (read_length > 0) {
        child_output += std::string(buffer, read_length);
      } else {
        break;
      }
    }
    close(test_pipe[0]);

    // Just make sure we can detect one of the calls in testing stack.
    std::string test_stack_frame = "testing::internal::UnitTestImpl::RunAllTests()";

    // Print the stack trace detected for information.
    std::cerr << "Output from the child process:";
    std::cerr << child_output;

    EXPECT_NE(child_output.find(test_stack_frame), std::string::npos);
  }
}


TEST_P(FailureSignalHandlerDeathTest, AbslFailureSignal) {
  const int signo = GetParam();
  std::string exit_regex = absl::StrCat(
      "\\*\\*\\* ", absl::debugging_internal::FailureSignalToString(signo),
      " received at time=");
  
  EXPECT_DEATH(InstallHandlerAndRaise(signo), exit_regex);
}

constexpr int kFailureSignals[] = {
    SIGSEGV, SIGILL,  SIGFPE, SIGABRT, SIGTERM,
};

std::string SignalParamToString(const ::testing::TestParamInfo<int>& info) {
  std::string result =
      absl::debugging_internal::FailureSignalToString(info.param);
  if (result.empty()) {
    result = absl::StrCat(info.param);
  }
  return result;
}

INSTANTIATE_TEST_SUITE_P(AbslDeathTest, FailureSignalHandlerDeathTest,
                         ::testing::ValuesIn(kFailureSignals),
                         SignalParamToString);

#endif  // GTEST_HAS_DEATH_TEST

}  // namespace

int main(int argc, char** argv) {
  // absl::InitializeSymbolizer(argv[0]);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
