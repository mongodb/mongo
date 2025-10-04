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
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>

#include <jsapi.h>

#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Reads into a JS Value from some Mongo C++ primitive
 */
class MONGO_MOD_PUB ValueReader {
public:
    /**
     * Depth is used when readers are invoked from ObjectWrappers to avoid
     * reading out overly nested objects
     */
    ValueReader(JSContext* cx, JS::MutableHandleValue value);

    void fromBSONElement(const BSONElement& elem, const BSONObj& parent, bool readOnly);
    void fromBSON(const BSONObj& obj, const BSONObj* parent, bool readOnly);
    void fromBSONArray(const BSONObj& obj, const BSONObj* parent, bool readOnly);
    void fromDouble(double d);
    void fromInt64(int64_t i);
    void fromStringData(StringData sd);
    void fromDecimal128(Decimal128 decimal);

private:
    JSContext* _context;
    JS::MutableHandleValue _value;
};

}  // namespace mozjs
}  // namespace mongo
