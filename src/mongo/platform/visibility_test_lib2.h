// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/platform/visibility.h"
#include "mongo/platform/visibility_test_lib1.h"
#include "mongo/util/modules.h"

#include <string>

#ifdef MONGO_API_visibility_test_lib2
#define MONGO_API_VISIBILITY_TEST_LIB2 MONGO_API_EXPORT
#else
#define MONGO_API_VISIBILITY_TEST_LIB2 MONGO_API_IMPORT
#endif

namespace mongo {
namespace visibility_test_lib2 {

class MONGO_API_VISIBILITY_TEST_LIB2 Derived : public visibility_test_lib1::Base {
public:
    explicit Derived(const std::string& name, double value);
    double value() const;

private:
    MONGO_PRIVATE static void _validate(double);

    double _value;
};

}  // namespace visibility_test_lib2
}  // namespace mongo
