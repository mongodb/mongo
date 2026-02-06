// Copyright 2023 The Centipede Authors.
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

#include "./centipede/symbol_table.h"

#include <sstream>
#include <string>

#include "gtest/gtest.h"

namespace fuzztest::internal {
namespace {

TEST(SymbolTableTest, SerializesAndDeserializesCorrectly) {
  std::string input =
      R"(FunctionOne
    source/location/one.cc:1:0

    FunctionTwo
    source/location/two.cc:2:0

)";
  std::istringstream input_stream(input);
  SymbolTable symbol_table;

  symbol_table.ReadFromLLVMSymbolizer(input_stream);

  std::ostringstream output_stream;
  symbol_table.WriteToLLVMSymbolizer(output_stream);
  EXPECT_EQ(input, output_stream.str());
}

TEST(SymbolTableTest, SerializesAndDeserializesCorrectlyWithUnknownFile) {
  std::string input =
      R"(?
    ?

)";
  std::istringstream input_stream(input);
  SymbolTable symbol_table;

  symbol_table.ReadFromLLVMSymbolizer(input_stream);

  std::ostringstream output_stream;
  symbol_table.WriteToLLVMSymbolizer(output_stream);
  EXPECT_EQ(input, output_stream.str());
}

TEST(SymbolTableTest, SerializesEmptyOutput) {
  std::string input =
      R"(
)";
  std::istringstream input_stream(input);
  SymbolTable symbol_table;

  symbol_table.ReadFromLLVMSymbolizer(input_stream);
}

}  // namespace
}  // namespace fuzztest::internal
