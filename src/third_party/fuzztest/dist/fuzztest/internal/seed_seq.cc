// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./fuzztest/internal/seed_seq.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/random/random.h"
#include "absl/strings/escaping.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "./fuzztest/internal/logging.h"

namespace fuzztest::internal {
namespace {

std::vector<uint32_t> MakeSeedMaterial() {
  absl::BitGen gen;
  static constexpr int kNumberOfEntropyBits = 256;
  std::vector<uint32_t> seed_material(kNumberOfEntropyBits /
                                      (8 * sizeof(uint32_t)));
  std::generate(seed_material.begin(), seed_material.end(),
                [&gen] { return absl::Uniform<uint32_t>(gen); });
  return seed_material;
}

std::vector<uint32_t> GetFromEnvOrMakeSeedMaterial(absl::string_view env_var) {
  const char* encoded_seed_material = std::getenv(env_var.data());
  if (encoded_seed_material == nullptr) {
    return MakeSeedMaterial();
  }
  std::optional<std::vector<uint32_t>> seed_material =
      DecodeSeedMaterial(encoded_seed_material);
  FUZZTEST_INTERNAL_CHECK_PRECONDITION(
      seed_material.has_value(),
      "Failed to decode seed material from the environment variable ", env_var);
  return *std::move(seed_material);
}

}  // namespace

std::seed_seq GetFromEnvOrMakeSeedSeq(std::ostream& out,
                                      absl::string_view env_var) {
  const std::vector<uint32_t> seed_material =
      GetFromEnvOrMakeSeedMaterial(env_var);
  const std::string encoded_seed_material = EncodeSeedMaterial(seed_material);
  out << env_var << "=" << encoded_seed_material << '\n';
  return std::seed_seq(seed_material.begin(), seed_material.end());
}

std::string EncodeSeedMaterial(absl::Span<const uint32_t> seed_material) {
  return absl::WebSafeBase64Escape(
      absl::string_view(reinterpret_cast<const char*>(seed_material.data()),
                        seed_material.size() * sizeof(uint32_t)));
}

std::optional<std::vector<uint32_t>> DecodeSeedMaterial(
    absl::string_view seed_material) {
  std::string unescaped;
  if (!absl::WebSafeBase64Unescape(seed_material, &unescaped))
    return std::nullopt;

  static constexpr auto kBlockSize = sizeof(uint32_t);
  std::vector<uint32_t> decoded_seed_material(
      (unescaped.size() + kBlockSize - 1) / kBlockSize);
  memcpy(&decoded_seed_material[0], unescaped.data(), unescaped.size());
  return decoded_seed_material;
}

}  // namespace fuzztest::internal
