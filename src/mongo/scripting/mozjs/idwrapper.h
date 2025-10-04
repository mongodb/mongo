/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>

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
    StringData toStringData(JSStringWrapper* jsstr) const;

    /**
     * Converts to an int.  This throws if the id is not an integer
     */
    uint32_t toInt32() const;

    void toValue(JS::MutableHandleValue value) const;

    bool isString() const;
    bool isInt() const;

    bool equals(StringData sd) const;
    bool equalsAscii(StringData sd) const;

private:
    JSContext* _context;
    JS::RootedId _value;
};

}  // namespace mozjs
}  // namespace mongo
