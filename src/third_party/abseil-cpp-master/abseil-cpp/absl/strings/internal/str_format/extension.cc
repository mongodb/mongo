//
// Copyright 2017 The Abseil Authors.
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

#include "absl/strings/internal/str_format/extension.h"

#include <errno.h>
#include <algorithm>
#include <string>

namespace absl {
namespace str_format_internal {
namespace {
// clang-format off
#define ABSL_LENGTH_MODS_EXPAND_ \
  X_VAL(h) X_SEP \
  X_VAL(hh) X_SEP \
  X_VAL(l) X_SEP \
  X_VAL(ll) X_SEP \
  X_VAL(L) X_SEP \
  X_VAL(j) X_SEP \
  X_VAL(z) X_SEP \
  X_VAL(t) X_SEP \
  X_VAL(q)
// clang-format on
}  // namespace

const LengthMod::Spec LengthMod::kSpecs[] = {
#define X_VAL(id) { LengthMod::id, #id, strlen(#id) }
#define X_SEP ,
    ABSL_LENGTH_MODS_EXPAND_, {LengthMod::none, "", 0}
#undef X_VAL
#undef X_SEP
};

const ConversionChar::Spec ConversionChar::kSpecs[] = {
#define X_VAL(id) { ConversionChar::id, #id[0] }
#define X_SEP ,
    ABSL_CONVERSION_CHARS_EXPAND_(X_VAL, X_SEP),
    {ConversionChar::none, '\0'},
#undef X_VAL
#undef X_SEP
};

std::string Flags::ToString() const {
  std::string s;
  s.append(left     ? "-" : "");
  s.append(show_pos ? "+" : "");
  s.append(sign_col ? " " : "");
  s.append(alt      ? "#" : "");
  s.append(zero     ? "0" : "");
  return s;
}

const size_t LengthMod::kNumValues;

const size_t ConversionChar::kNumValues;

bool FormatSinkImpl::PutPaddedString(string_view v, int w, int p, bool l) {
  size_t space_remaining = 0;
  if (w >= 0) space_remaining = w;
  size_t n = v.size();
  if (p >= 0) n = std::min(n, static_cast<size_t>(p));
  string_view shown(v.data(), n);
  space_remaining = Excess(shown.size(), space_remaining);
  if (!l) Append(space_remaining, ' ');
  Append(shown);
  if (l) Append(space_remaining, ' ');
  return true;
}

}  // namespace str_format_internal
}  // namespace absl
