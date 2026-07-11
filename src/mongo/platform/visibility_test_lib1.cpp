// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/visibility_test_lib1.h"

#include "mongo/platform/visibility_test_libcommon.h"

#include <stdexcept>
#include <string_view>

namespace mongo {
namespace visibility_test_lib1 {

Base::Base(const std::string& name) : _name(name) {
    _validate(_name);
}

std::string_view Base::name() const {
    return _name;
}

void Base::_validate(std::string_view name) {
    if (!visibility_test_libcommon::validate(name))
        throw std::logic_error("Invalid name");
}

}  // namespace visibility_test_lib1
}  // namespace mongo
