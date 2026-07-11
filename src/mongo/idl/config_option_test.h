// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <cstdint>

namespace mongo {
namespace test {

extern bool gEnableTestConfigOpt14;
extern bool gEnableTestConfigOpt15;

constexpr std::int32_t kTestConfigOpt17Default = 42;
constexpr std::int32_t kTestConfigOpt17Implicit = 43;
constexpr std::int32_t kTestConfigOpt17Minimum = 5;
constexpr std::int32_t kTestConfigOpt17Maximum = 100;

}  // namespace test
}  // namespace mongo
