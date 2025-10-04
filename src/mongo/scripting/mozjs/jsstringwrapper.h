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
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <jsapi.h>

#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Wraps JSStrings to simplify coercing them to and from C++ style StringData
 * and std::strings.
 */
class JSStringWrapper {
public:
    JSStringWrapper() = default;
    JSStringWrapper(JSContext* cx, JSString* str);
    JSStringWrapper(std::int32_t val);

    JSStringWrapper(JSStringWrapper&&) = default;

    JSStringWrapper& operator=(JSStringWrapper&&) = default;

    StringData toStringData() const;
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
