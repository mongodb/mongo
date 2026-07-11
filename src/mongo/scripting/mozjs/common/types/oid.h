// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/oid.h"
#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/CallArgs.h>
#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * The "ObjectId" Javascript object.
 *
 * Holds a private bson OID
 */
struct OIDInfo : public BaseInfo {
    enum Slots { OIDSlot, OIDInfoSlotCount };

    static void construct(JSContext* cx, JS::CallArgs args);
    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(getter);
        MONGO_DECLARE_JS_FUNCTION(toString);
        MONGO_DECLARE_JS_FUNCTION(toJSON);
    };

    static const JSFunctionSpec methods[3];

    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(OIDInfoSlotCount) | BaseInfo::finalizeFlag;
    static const char* const className;

    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);

    static void make(JSContext* cx, const OID& oid, JS::MutableHandleValue out);
    static OID getOID(JSContext* cx, JS::HandleValue value);
    static OID getOID(JSContext* cx, JS::HandleObject object);
};

}  // namespace mozjs
}  // namespace mongo
