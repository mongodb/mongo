// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/visibility_test_libcommon.h"

#include <string>
#include <string_view>

namespace mongo {
namespace visibility_test_libcommon {

namespace {
const std::string_view kInvalidStringView = "invalid";
const double kInvalidDouble = -1;
}  // namespace

bool validate(std::string_view data) {
    return data != kInvalidStringView;
}

bool validate(double data) {
    return data != kInvalidDouble;
}

}  // namespace visibility_test_libcommon
}  // namespace mongo
