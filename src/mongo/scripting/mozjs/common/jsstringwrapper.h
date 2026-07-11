// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <jsapi.h>

#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Wraps JSStrings to simplify coercing them to and from C++ style std::string_view
 * and std::strings.
 */
class JSStringWrapper {
public:
    JSStringWrapper() = default;
    JSStringWrapper(JSContext* cx, JSString* str);
    JSStringWrapper(std::int32_t val);

    JSStringWrapper(JSStringWrapper&&) = default;

    JSStringWrapper& operator=(JSStringWrapper&&) = default;

    std::string_view toStringData() const;
    std::string toString() const;

    explicit operator bool() const;

private:
    std::unique_ptr<char[]> _str;
    size_t _length = 0;
    char _buf[64];
    bool _isSet = false;
};

}  // namespace mozjs
}  // namespace mongo
