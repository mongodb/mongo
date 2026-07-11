// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/visibility.h"
#include "mongo/util/modules.h"

#include <string_view>

#ifdef MONGO_API_visibility_test_libcommon
#define MONGO_VISIBILITY_TEST_LIBCOMMON_API MONGO_API_EXPORT
#else
#define MONGO_VISIBILITY_TEST_LIBCOMMON_API MONGO_API_IMPORT
#endif

namespace mongo {
namespace visibility_test_libcommon {

MONGO_VISIBILITY_TEST_LIBCOMMON_API bool validate(std::string_view data);
MONGO_VISIBILITY_TEST_LIBCOMMON_API bool validate(double data);

}  // namespace visibility_test_libcommon
}  // namespace mongo
