// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/platform/visibility.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#ifdef MONGO_API_visibility_test_lib1
#define MONGO_VISIBILITY_TEST_LIB1_API MONGO_API_EXPORT
#else
#define MONGO_VISIBILITY_TEST_LIB1_API MONGO_API_IMPORT
#endif

namespace mongo {
namespace visibility_test_lib1 {
class MONGO_VISIBILITY_TEST_LIB1_API Base {
public:
    explicit Base(const std::string& name);
    std::string_view name() const;

private:
    MONGO_PRIVATE static void _validate(std::string_view);

    std::string _name;
};

}  // namespace visibility_test_lib1
}  // namespace mongo
