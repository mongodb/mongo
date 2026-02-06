// Copyright 2022 The Centipede Authors.
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

#include "./centipede/reverse_pc_table.h"

#include "gtest/gtest.h"
#include "./centipede/pc_info.h"

namespace fuzztest::internal {
namespace {

TEST(ReversePCTable, ReversePCTable) {
  static ReversePCTable table;
  const PCTable pc_table = {{500, PCInfo::kFuncEntry},
                            {400, 0},
                            {100, PCInfo::kFuncEntry},
                            {200, 0},
                            {300, 0}};
  table.SetFromPCs(pc_table);

  EXPECT_EQ(table.NumPcs(), 5);
  EXPECT_FALSE(table.GetPCGuard(0).IsValid());
  EXPECT_FALSE(table.GetPCGuard(50).IsValid());
  EXPECT_FALSE(table.GetPCGuard(150).IsValid());
  EXPECT_FALSE(table.GetPCGuard(501).IsValid());

  EXPECT_EQ(table.GetPCGuard(500).pc_index, 0);
  EXPECT_TRUE(table.GetPCGuard(500).is_function_entry);
  EXPECT_EQ(table.GetPCGuard(400).pc_index, 1);
  EXPECT_FALSE(table.GetPCGuard(400).is_function_entry);
  EXPECT_EQ(table.GetPCGuard(100).pc_index, 2);
  EXPECT_TRUE(table.GetPCGuard(100).is_function_entry);
  EXPECT_EQ(table.GetPCGuard(200).pc_index, 3);
  EXPECT_FALSE(table.GetPCGuard(200).is_function_entry);
  EXPECT_EQ(table.GetPCGuard(300).pc_index, 4);
  EXPECT_FALSE(table.GetPCGuard(300).is_function_entry);

  // Reset the table and try new values.
  const PCTable pc_table1 = {{40, 0}, {20, 0}, {30, 0}};
  table.SetFromPCs(pc_table1);
  EXPECT_FALSE(table.GetPCGuard(200).IsValid());
  EXPECT_EQ(table.GetPCGuard(40).pc_index, 0);
  EXPECT_EQ(table.GetPCGuard(20).pc_index, 1);
  EXPECT_EQ(table.GetPCGuard(30).pc_index, 2);
}

}  // namespace
}  // namespace fuzztest::internal
