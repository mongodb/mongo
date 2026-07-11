// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/shared_library.h"

#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {

typedef StatusWith<void (*)()> StatusWithFunc;

SharedLibrary::SharedLibrary(void* handle) : _handle(handle) {}

StatusWithFunc SharedLibrary::getFunction(std::string_view name) {
    StatusWith<void*> s = getSymbol(name);

    if (!s.isOK()) {
        return StatusWithFunc(s.getStatus());
    }

    return StatusWithFunc(reinterpret_cast<void (*)()>(s.getValue()));
}

}  // namespace mongo
