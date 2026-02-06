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

#include "./centipede/pc_info.h"

#include <cstddef>
#include <istream>
#include <iterator>
#include <ostream>
#include <string>

#include "absl/log/check.h"
#include "absl/types/span.h"
#include "./common/defs.h"

namespace fuzztest::internal {

bool PCInfo::operator==(const PCInfo &rhs) const {
  return this->pc == rhs.pc && this->flags == rhs.flags;
}

PCTable ReadPcTable(std::istream &in) {
  std::string input_string(std::istreambuf_iterator<char>(in), {});

  ByteArray pc_infos_as_bytes(input_string.begin(), input_string.end());
  CHECK_EQ(pc_infos_as_bytes.size() % sizeof(PCInfo), 0);
  size_t pc_table_size = pc_infos_as_bytes.size() / sizeof(PCInfo);
  const auto *pc_infos = reinterpret_cast<PCInfo *>(pc_infos_as_bytes.data());
  PCTable pc_table{pc_infos, pc_infos + pc_table_size};
  CHECK_EQ(pc_table.size(), pc_table_size);

  return pc_table;
}

void WritePcTable(const PCTable &pc_table, std::ostream &out) {
  auto pc_infos_as_bytes =
      absl::Span<const char>(reinterpret_cast<const char *>(pc_table.data()),
                             sizeof(PCInfo) * pc_table.size());
  out.write(pc_infos_as_bytes.data(), pc_infos_as_bytes.size());
}

}  // namespace fuzztest::internal
