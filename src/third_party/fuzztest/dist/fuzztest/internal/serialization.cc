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

#include "./fuzztest/internal/serialization.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"

namespace fuzztest::internal {

namespace {

struct OutputVisitor {
  size_t index;
  int indent;
  std::string& out;

  void operator()(std::monostate) const {}

  void operator()(uint64_t value) const { absl::StrAppend(&out, "i: ", value); }

  void operator()(double value) const {
    // Print with the maximum precision necessary to prevent losses.
    absl::StrAppendFormat(&out, "d: %.*g",
                          std::numeric_limits<double>::max_digits10, value);
  }

  void operator()(const std::string& value) const {
    absl::StrAppend(&out, "s: \"");
    for (char c : value) {
      if (std::isprint(c) && c != '\\' && c != '"') {
        out.append(1, c);
      } else {
        absl::StrAppendFormat(&out, "\\%03o", c);
      }
    }
    absl::StrAppend(&out, "\"");
  }

  void operator()(const std::vector<IRObject>& value) const {
    for (const auto& sub : value) {
      const bool sub_is_scalar =
          !std::holds_alternative<std::vector<IRObject>>(sub.value);
      absl::StrAppendFormat(&out, "%*ssub {%s", indent, "",
                            sub_is_scalar ? " " : "\n");
      std::visit(OutputVisitor{sub.value.index(), indent + 2, out}, sub.value);
      absl::StrAppendFormat(&out, "%*s}\n", sub_is_scalar ? 0 : indent,
                            sub_is_scalar ? " " : "");
    }
  }
};

constexpr absl::string_view kHeader = "FUZZTESTv1";

absl::string_view ReadToken(absl::string_view& in) {
  while (!in.empty() && std::isspace(in[0])) in.remove_prefix(1);
  if (in.empty()) return in;
  size_t end = 1;
  const auto is_literal = [](char c) {
    return std::isalnum(c) != 0 || c == '+' || c == '-' || c == '.';
  };
  if (is_literal(in[0])) {
    while (end < in.size() && is_literal(in[end])) ++end;
  } else if (in[0] == '"') {
    while (end < in.size() && in[end] != '"') ++end;
    if (end < in.size()) ++end;
  }
  absl::string_view res = in.substr(0, end);
  in.remove_prefix(end);
  return res;
}

bool ReadScalar(uint64_t& out, absl::string_view value) {
  return absl::SimpleAtoi(value, &out);
}

bool ReadScalar(double& out, absl::string_view value) {
  return absl::SimpleAtod(value, &out);
}

bool ReadScalar(std::string& out, absl::string_view value) {
  if (value.empty() || value[0] != '"') return false;
  value.remove_prefix(1);

  if (value.empty() || value.back() != '"') return false;
  value.remove_suffix(1);

  while (!value.empty()) {
    if (value[0] != '\\') {
      out += value[0];
      value.remove_prefix(1);
    } else {
      uint32_t v = 0;

      if (value.size() < 4) return false;
      for (int i = 1; i < 4; ++i) {
        if (value[i] < '0' || value[i] > '7') {
          return false;
        }
        v = 8 * v + value[i] - '0';
      }
      if (v > 255) return false;

      out += static_cast<char>(v);
      value.remove_prefix(4);
    }
  }
  return true;
}

constexpr int kMaxParseRecursionDepth = 128;

bool ParseImpl(IRObject& obj, absl::string_view& str, int recursion_depth) {
  if (recursion_depth > kMaxParseRecursionDepth) return false;
  absl::string_view key = ReadToken(str);
  if (key.empty() || key == "}") {
    // The object is empty. Put the token back and return.
    str = absl::string_view(key.data(), str.data() + str.size() - key.data());
    return true;
  }

  if (key == "sub") {
    auto& v = obj.value.emplace<std::vector<IRObject>>();
    do {
      if (ReadToken(str) != "{") return false;
      if (!ParseImpl(v.emplace_back(), str, recursion_depth + 1)) return false;
      if (ReadToken(str) != "}") return false;
      key = ReadToken(str);
    } while (key == "sub");
    // We are done reading this repeated sub.
    // Put the token back for the caller.
    str = absl::string_view(key.data(), str.data() + str.size() - key.data());
    return true;
  } else {
    if (ReadToken(str) != ":") return false;
    auto value = ReadToken(str);
    auto& v = obj.value;
    if (key == "i") {
      return ReadScalar(v.emplace<uint64_t>(), value);
    } else if (key == "d") {
      return ReadScalar(v.emplace<double>(), value);
    } else if (key == "s") {
      return ReadScalar(v.emplace<std::string>(), value);
    } else {
      // Unrecognized key
      return false;
    }
  }
}

// NOTE: Binary format assumes the same endianness between producers and
// consumers - we assume little-endianness for now.

enum class BinaryFormatHeader : char {
  kEmpty = 0,
  kUInt64,
  kDouble,
  kString,
  kObject,
};

struct BinaryOutputVisitor {
  char* buf;
  size_t& offset;

  void operator()(std::monostate) const {
    if (buf) {
      buf[offset] = static_cast<char>(BinaryFormatHeader::kEmpty);
    }
    offset += 1;
  }

  void operator()(uint64_t value) const {
    if (buf) {
      buf[offset] = static_cast<char>(BinaryFormatHeader::kUInt64);
      std::memcpy(buf + offset + 1, &value, sizeof(value));
    }
    offset += 1 + sizeof(value);
  }

  void operator()(double value) const {
    if (buf) {
      buf[offset] = static_cast<char>(BinaryFormatHeader::kDouble);
      std::memcpy(buf + offset + 1, &value, sizeof(value));
    }
    offset += 1 + sizeof(value);
  }

  void operator()(const std::string& value) const {
    const uint64_t size = value.size();
    if (buf) {
      buf[offset] = static_cast<char>(BinaryFormatHeader::kString);
      std::memcpy(buf + offset + 1, &size, sizeof(size));
      std::memcpy(buf + offset + 1 + sizeof(size), value.data(), size);
    }
    offset += 1 + sizeof(size) + size;
  }

  void operator()(const std::vector<IRObject>& value) const {
    const uint64_t size = value.size();
    if (buf) {
      buf[offset] = static_cast<char>(BinaryFormatHeader::kObject);
      std::memcpy(buf + offset + 1, &size, sizeof(size));
    }
    offset += 1 + sizeof(size);
    for (const auto& sub : value) {
      std::visit(BinaryOutputVisitor{buf, offset}, sub.value);
    }
  }
};

constexpr absl::string_view kBinaryHeader = "FUZZTESTv1b";

struct BinaryParseBuf {
  const char* str;
  size_t size;

  inline bool empty() const { return size == 0; }
  inline void Advance(size_t s) {
    if (s > size) s = size;
    str += s;
    size -= s;
  }
};

bool BinaryParse(IRObject& obj, BinaryParseBuf& buf, int recursion_depth) {
  if (recursion_depth > kMaxParseRecursionDepth) return false;
  if (buf.empty()) return false;
  const auto h = static_cast<BinaryFormatHeader>(buf.str[0]);
  buf.Advance(1);
  switch (h) {
    case BinaryFormatHeader::kEmpty: {
      return true;
    }
    case BinaryFormatHeader::kUInt64: {
      if (buf.size < sizeof(uint64_t)) return false;
      auto& t = obj.value.emplace<uint64_t>();
      std::memcpy(&t, buf.str, sizeof(uint64_t));
      buf.Advance(sizeof(uint64_t));
      return true;
    }
    case BinaryFormatHeader::kDouble: {
      if (buf.size < sizeof(double)) return false;
      auto& t = obj.value.emplace<double>();
      std::memcpy(&t, buf.str, sizeof(t));
      buf.Advance(sizeof(double));
      return true;
    }
    case BinaryFormatHeader::kString: {
      if (buf.size < sizeof(uint64_t)) return false;
      uint64_t str_size;
      std::memcpy(&str_size, buf.str, sizeof(str_size));
      buf.Advance(sizeof(uint64_t));
      if (buf.size < str_size) return false;
      obj.value.emplace<std::string>() = {buf.str,
                                          static_cast<size_t>(str_size)};
      buf.Advance(str_size);
      return true;
    }
    case BinaryFormatHeader::kObject: {
      if (buf.size < sizeof(uint64_t)) return false;
      uint64_t vec_size;
      std::memcpy(&vec_size, buf.str, sizeof(vec_size));
      buf.Advance(sizeof(vec_size));
      // This could happen for malformed inputs.
      if (vec_size > buf.size) return false;
      auto& v = obj.value.emplace<std::vector<IRObject>>();
      v.reserve(vec_size);
      for (uint64_t i = 0; i < vec_size; ++i) {
        if (!BinaryParse(v.emplace_back(), buf, recursion_depth + 1))
          return false;
      }
      return true;
    }
  }
  return false;
}

bool IsInBinaryFormat(absl::string_view str) {
  // Not using absl::string_view or std::memcmp because they could be
  // instrumented and using them could pollute coverage.
  return str.size() >= kBinaryHeader.size() &&
         __builtin_memcmp(str.data(), kBinaryHeader.data(),
                          kBinaryHeader.size()) == 0;
}

}  // namespace

std::string IRObject::ToString(bool binary_format) const {
  if (binary_format) {
    size_t offset = kBinaryHeader.size();
    // Determine the output size before writing to the output to avoid
    // reallocation.
    std::visit(BinaryOutputVisitor{/*buf=*/nullptr, offset}, value);
    std::string out;
    out.resize(offset);
    std::memcpy(out.data(), kBinaryHeader.data(), kBinaryHeader.size());
    offset = kBinaryHeader.size();
    std::visit(BinaryOutputVisitor{out.data(), offset}, value);
    return out;
  }
  std::string out = absl::StrCat(kHeader, "\n");
  std::visit(OutputVisitor{value.index(), 0, out}, value);
  return out;
}

// TODO(lszekeres): Return StatusOr<IRObject>.
std::optional<IRObject> IRObject::FromString(absl::string_view str) {
  IRObject object;
  if (IsInBinaryFormat(str)) {
    BinaryParseBuf buf = {str.data(), str.size()};
    buf.Advance(kBinaryHeader.size());
    if (!BinaryParse(object, buf, /*recursion_depth=*/0) || !buf.empty())
      return std::nullopt;
  } else {
    if (ReadToken(str) != kHeader) return std::nullopt;
    if (!ParseImpl(object, str, /*recursion_depth=*/0) ||
        !ReadToken(str).empty())
      return std::nullopt;
  }
  return object;
}

}  // namespace fuzztest::internal
