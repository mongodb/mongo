// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/platform/decimal128.h"
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
 * Reads into a JS Value from some Mongo C++ primitive
 */
class [[MONGO_MOD_PUBLIC]] ValueReader {
public:
    /**
     * Depth is used when readers are invoked from ObjectWrappers to avoid
     * reading out overly nested objects
     */
    ValueReader(JSContext* cx, JS::MutableHandleValue value);

    void fromBSONElement(const BSONElement& elem, const BSONObj& parent, bool readOnly);
    // Wraps elem without a parent, making the resulting BSONHolder unowned.
    // Unowned holders always check the generation counter in uassertValid(), so
    // any holder created here becomes stale after the next reset()/advanceGeneration().
    void fromBSONElementUnowned(const BSONElement& elem, bool readOnly);
    void fromBSON(const BSONObj& obj, const BSONObj* parent, bool readOnly);
    void fromBSONArray(const BSONObj& obj, const BSONObj* parent, bool readOnly);
    void fromDouble(double d);
    void fromInt64(int64_t i);
    void fromStringData(std::string_view sd);
    void fromDecimal128(Decimal128 decimal);

private:
    JSContext* _context;
    JS::MutableHandleValue _value;
};

}  // namespace mozjs
}  // namespace mongo
