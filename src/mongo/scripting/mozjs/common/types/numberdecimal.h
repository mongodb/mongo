// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/decimal128.h"
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
 * The "NumberDecimal" Javascript object.
 *
 * Wraps a 'Decimal128' as its private member
 */

struct [[MONGO_MOD_PUBLIC]] NumberDecimalInfo : public BaseInfo {
    enum Slots { Decimal128Slot, NumberDecimalInfoSlotCount };

    static void construct(JSContext* cx, JS::CallArgs args);
    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(toString);
        MONGO_DECLARE_JS_FUNCTION(toJSON);
    };

    static const JSFunctionSpec methods[3];

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(NumberDecimalInfoSlotCount) | BaseInfo::finalizeFlag;
    static Decimal128 ToNumberDecimal(JSContext* cx, JS::HandleObject object);
    static Decimal128 ToNumberDecimal(JSContext* cx, JS::HandleValue value);

    static void make(JSContext* cx, JS::MutableHandleValue value, Decimal128 d);
};

}  // namespace mozjs
}  // namespace mongo
