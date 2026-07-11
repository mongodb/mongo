// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

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
 * Wrapper for the BinData bson type
 *
 * It offers some simple methods and a handful of specialized constructors
 */
struct [[MONGO_MOD_PUBLIC]] BinDataInfo : public BaseInfo {
    enum Slots { BinDataStringSlot, BinDataSlotCount };

    static void construct(JSContext* cx, JS::CallArgs args);
    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(base64);
        MONGO_DECLARE_JS_FUNCTION(hex);
        MONGO_DECLARE_JS_FUNCTION(toString);
        MONGO_DECLARE_JS_FUNCTION(toJSON);

        MONGO_DECLARE_JS_FUNCTION(HexData);
        MONGO_DECLARE_JS_FUNCTION(MD5);
        MONGO_DECLARE_JS_FUNCTION(UUID);
    };

    static const JSFunctionSpec methods[5];
    static const JSFunctionSpec freeFunctions[4];

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(BinDataSlotCount) | BaseInfo::finalizeFlag;
};

}  // namespace mozjs
}  // namespace mongo
