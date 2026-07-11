// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/Class.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * The "MongoStatus" Javascript object.
 *
 * This type wraps the "Status" type in the server, allowing for lossless throwing of mongodb native
 * exceptions through javascript.  It can be created (albeit without sidecar) from javascript.
 * These are also created automatically when exceptions are thrown from native c++ functions.
 *
 * They are somewhat special, in that the prototype for each MongoStatus object is actually an Error
 * object specific to that status object.  This allows Error-like behavior such as useful stack
 * traces, and instanceOf Error.
 */
struct MongoStatusInfo : public BaseInfo {
    enum Slots { StatusSlot, MongoStatusInfoSlotCount };

    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(code);
        MONGO_DECLARE_JS_FUNCTION(reason);
        MONGO_DECLARE_JS_FUNCTION(stack);
    };

    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);

    static const char* const className;
    static const char* const inheritFrom;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(MongoStatusInfoSlotCount) | BaseInfo::finalizeFlag;
    static const InstallType installType = InstallType::Private;

    static Status toStatus(JSContext* cx, JS::HandleObject object);
    static Status toStatus(JSContext* cx, JS::HandleValue value);
    static void fromStatus(JSContext* cx, Status status, JS::MutableHandleValue value);
};

}  // namespace mozjs
}  // namespace mongo
