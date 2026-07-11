// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <tuple>

#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Provides a wrapper for BSONObj's in JS. The main idea here is that BSONObj's
 * can be read only, or read write when we shim them in, and in all cases we
 * lazily load their member elements into JS. So a bunch of these lifecycle
 * methods are set up to wrap field access (enumerate, resolve and
 * del/setProperty).
 *
 * Note that installType is private. So you can only get BSON types in JS via
 * ::make() from C++.
 */
struct BSONInfo : public BaseInfo {
    enum Slots { BSONHolderSlot, BSONInfoSlotCount };

    static void delProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::ObjectOpResult& result);
    static void enumerate(JSContext* cx,
                          JS::HandleObject obj,
                          JS::MutableHandleIdVector properties,
                          bool enumerableOnly);
    static void finalize(JS::GCContext* gcCtx, JSObject* obj);
    static void resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp);
    static void setProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::HandleValue v,
                            JS::HandleValue receiver,
                            JS::ObjectOpResult& result);

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(BSONInfoSlotCount) | BaseInfo::finalizeFlag;
    static const InstallType installType = InstallType::Private;
    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(bsonWoCompare);
        MONGO_DECLARE_JS_FUNCTION(bsonUnorderedFieldsCompare);
        MONGO_DECLARE_JS_FUNCTION(bsonBinaryEqual);
        MONGO_DECLARE_JS_FUNCTION(bsonObjToArray);
        MONGO_DECLARE_JS_FUNCTION(bsonToBase64);
        MONGO_DECLARE_JS_FUNCTION(bsonGetImmutable);
    };

    static const JSFunctionSpec freeFunctions[7];

    static std::tuple<BSONObj*, bool> originalBSON(JSContext* cx, JS::HandleObject obj);
    static void make(
        JSContext* cx, JS::MutableHandleObject obj, BSONObj bson, const BSONObj* parent, bool ro);
};

}  // namespace mozjs
}  // namespace mongo
