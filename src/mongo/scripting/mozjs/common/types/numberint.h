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
 * The "NumberInt" Javascript object.
 *
 * Wraps an actual c++ 'int' as its private member
 */
struct NumberIntInfo : public BaseInfo {
    enum Slots { IntSlot, NumberIntInfoSlotCount };

    static void construct(JSContext* cx, JS::CallArgs args);
    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(toNumber);
        MONGO_DECLARE_JS_FUNCTION(toString);
        MONGO_DECLARE_JS_FUNCTION(toJSON);
        MONGO_DECLARE_JS_FUNCTION(valueOf);
    };

    static const JSFunctionSpec methods[5];

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(NumberIntInfoSlotCount) | BaseInfo::finalizeFlag;

    static int ToNumberInt(JSContext* cx, JS::HandleObject object);
    static int ToNumberInt(JSContext* cx, JS::HandleValue value);
};

}  // namespace mozjs
}  // namespace mongo
