#ifndef PARLAY_TEST_TEMPORARY_FILE_H_
#define PARLAY_TEST_TEMPORARY_FILE_H_

#include <cstdio>
#include <random>
#include <string>

#include "gtest/gtest.h"

// A file name of the running test's own, removed when the test ends.
//
// The tests that write files run as separate processes that share a working
// directory, several at a time. With a fixed name, one test truncates the file
// another has just written or mapped.
//
// Declare it before whatever opens the file, so that the file is closed first.
struct temporary_file {
  explicit temporary_file(const char* tag) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    name = std::string(info->test_suite_name()) + "." + info->name() + "." + tag + "." +
           std::to_string(std::random_device{}()) + ".txt";
  }
  ~temporary_file() { std::remove(name.c_str()); }

  temporary_file(const temporary_file&) = delete;
  temporary_file& operator=(const temporary_file&) = delete;

  std::string name;
};

#endif  // PARLAY_TEST_TEMPORARY_FILE_H_
