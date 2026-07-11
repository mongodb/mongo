// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/visibility_test_lib2.h"

#include "mongo/platform/visibility_test_libcommon.h"

#include <stdexcept>

namespace mongo {
namespace visibility_test_lib2 {

Derived::Derived(const std::string& name, double value) : Base(name), _value(value) {
    _validate(_value);
}

double Derived::value() const {
    return _value;
}

void Derived::_validate(double value) {
    if (!visibility_test_libcommon::validate(value))
        throw std::logic_error("Invalid value");
}

}  // namespace visibility_test_lib2
}  // namespace mongo
