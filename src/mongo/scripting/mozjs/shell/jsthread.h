// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Helper for the JSThread javascript object
 *
 * The workflow is strange because we have a thing in javascript called a
 * JSThread, but we don't actually get to construct it. Instead, we have to
 * inject methods into that thing (via _threadInject) and hang our C++ thread
 * separately (via init() on that type).
 *
 * To manage lifetime, we just add a field into the injected object that's our
 * JSThread and add our holder in as our JSThread's private member.
 */
struct JSThreadInfo : public BaseInfo {
    enum Slots { JSThreadConfigSlot, JSThreadInfoSlotCount };

    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(init);
        MONGO_DECLARE_JS_FUNCTION(start);
        MONGO_DECLARE_JS_FUNCTION(join);
        MONGO_DECLARE_JS_FUNCTION(hasFailed);
        MONGO_DECLARE_JS_FUNCTION(currentStatus);
        MONGO_DECLARE_JS_FUNCTION(returnData);

        MONGO_DECLARE_JS_FUNCTION(_threadInject);
        MONGO_DECLARE_JS_FUNCTION(_scopedThreadInject);
    };

    /**
     * Note that this isn't meant to supply methods for JSThread, it's just
     * there to work with _threadInject. So the name isn't a mistake
     */
    static const JSFunctionSpec threadMethods[7];
    static const JSFunctionSpec freeFunctions[3];

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(JSThreadInfoSlotCount) | BaseInfo::finalizeFlag;
    static const InstallType installType = InstallType::Private;
};

}  // namespace mozjs
}  // namespace mongo
