// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/jsstringwrapper.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>
#include <string_view>

#include <jsapi.h>

#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Wraps jsid's to make them slightly easier to use
 *
 * As these own a JS::RootedId they're not movable or copyable
 *
 * IdWrapper should only be used on the stack, never in a heap allocation
 */
class IdWrapper {
public:
    IdWrapper(JSContext* cx, JS::HandleId id);

    /**
     * Converts to a string.  This coerces for integers
     */
    std::string toString() const;
    std::string_view toStringData(JSStringWrapper* jsstr) const;

    /**
     * Converts to an int.  This throws if the id is not an integer
     */
    uint32_t toInt32() const;

    void toValue(JS::MutableHandleValue value) const;

    bool isString() const;
    bool isInt() const;

    bool equals(std::string_view sd) const;
    bool equalsAscii(std::string_view sd) const;

private:
    JSContext* _context;
    JS::RootedId _value;
};

}  // namespace mozjs
}  // namespace mongo
