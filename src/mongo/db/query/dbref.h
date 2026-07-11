// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace dbref {
using namespace std::literals::string_view_literals;

constexpr std::string_view kDbFieldName = "$db"sv;
constexpr std::string_view kIdFieldName = "$id"sv;
constexpr std::string_view kRefFieldName = "$ref"sv;

}  // namespace dbref
}  // namespace mongo
