// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/mozjs/common/jsstringwrapper.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/util/modules.h"

namespace mongo {
struct JSRegEx;
}

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <jsapi.h>

#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Writes C++ values out of JS Values
 *
 * originalBSON is a hack to keep integer types in their original type when
 * they're read out, manipulated in js and saved back.
 */
class [[MONGO_MOD_PUBLIC]] ValueWriter {
public:
    ValueWriter(JSContext* cx, JS::HandleValue value);

    BSONObj toBSON();

    /**
     * These coercions flow through JS::To_X. I.e. they can call toString() or
     * toNumber()
     */
    std::string toString();
    std::string_view toStringData(JSStringWrapper* jsstr);
    int type();
    double toNumber();
    int32_t toInt32();
    int64_t toInt64();
    Decimal128 toDecimal128();
    bool toBoolean();
    OID toOID();
    // Note: The resulting BSONBinData is only valid within the scope of the 'withBinData' callback.
    void toBinData(std::function<void(const BSONBinData&)> withBinData);
    Timestamp toTimestamp();
    JSRegEx toRegEx();

    /**
     * Provides the type of the value. For objects, it fetches the class name if possible.
     */
    std::string typeAsString();

    /**
     * Writes the value into a bsonobjbuilder under the name in sd.
     *
     * We take WriteFieldRecursionFrames so we can push a new object on if the underlying
     * value is an object. This allows us to recurse without C++ stack frames.
     *
     * Look in toBSON on ObjectWrapper for the top of that loop.
     */
    void writeThis(BSONObjBuilder* b,
                   std::string_view sd,
                   ObjectWrapper::WriteFieldRecursionFrames* frames);

    void setOriginalBSON(BSONObj* obj);

private:
    /**
     * Writes the object into a bsonobjbuilder under the name in sd.
     */
    void _writeObject(BSONObjBuilder* b,
                      std::string_view sd,
                      ObjectWrapper::WriteFieldRecursionFrames* frames);

    JSContext* _context;
    JS::HandleValue _value;
    BSONObj* _originalParent;
};

}  // namespace mozjs
}  // namespace mongo
