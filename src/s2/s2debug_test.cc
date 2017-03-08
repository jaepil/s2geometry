// Copyright 2005 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// Author: ericv@google.com (Eric Veach)

#include "s2/s2debug.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

// This pair of tests expects the tests to be run in order (which they are),
// otherwise it won't test anything.

class S2DebugTest : public testing::Test {
 public:
  S2DebugTest() {}
  virtual ~S2DebugTest() {}
};

TEST_F(S2DebugTest, Restore_Part1) {
  EXPECT_EQ(google::DEBUG_MODE, FLAGS_s2debug);
  FLAGS_s2debug = !FLAGS_s2debug;
}

TEST_F(S2DebugTest, Restore_Part2) {
  // Verify that the flag value was automatically restored to the default.
  EXPECT_EQ(google::DEBUG_MODE, FLAGS_s2debug);
}