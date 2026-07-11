// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <jsapi.h>

#include <js/CallArgs.h>
#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * The "NumberLong" Javascript object.
 *
 * Represents a 64 integer with a JS representation like:
 *
 * {
 *     top         : Double,
 *     bottom      : Double,
 *     floatApprox : Double,
 * }
 *
 * Where top is the high 32 bits, bottom the low 32 bits and floatApprox a
 * floating point approximation.
 */
struct NumberLongInfo : public BaseInfo {
    enum Slots { Int64Slot, NumberLongInfoSlotCount };

    static void construct(JSContext* cx, JS::CallArgs args);
    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(toNumber);
        MONGO_DECLARE_JS_FUNCTION(toString);
        MONGO_DECLARE_JS_FUNCTION(toJSON);
        MONGO_DECLARE_JS_FUNCTION(valueOf);
        MONGO_DECLARE_JS_FUNCTION(compare);
        MONGO_DECLARE_JS_FUNCTION(floatApprox);
        MONGO_DECLARE_JS_FUNCTION(top);
        MONGO_DECLARE_JS_FUNCTION(bottom);
        MONGO_DECLARE_JS_FUNCTION(exactValueString);
    };

    static const JSFunctionSpec methods[6];

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(NumberLongInfoSlotCount) | BaseInfo::finalizeFlag;

    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);

    static int64_t ToNumberLong(JSContext* cx, JS::HandleObject object);
    static int64_t ToNumberLong(JSContext* cx, JS::HandleValue value);
};

}  // namespace mozjs
}  // namespace mongo
