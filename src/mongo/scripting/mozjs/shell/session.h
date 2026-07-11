// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <memory>

#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Wraps a Session in javascript
 *
 * Note that the install is private, so this class should only be constructible
 * from C++. Current callers are all via the Mongo object.
 */
struct SessionInfo : public BaseInfo {
    enum Slots { SessionHolderSlot, SessionInfoSlotCount };

    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(end);
        MONGO_DECLARE_JS_FUNCTION(getId);
        MONGO_DECLARE_JS_FUNCTION(getTxnState);
        MONGO_DECLARE_JS_FUNCTION(setTxnState);
        MONGO_DECLARE_JS_FUNCTION(getTxnNumber);
        MONGO_DECLARE_JS_FUNCTION(setTxnNumber);
        MONGO_DECLARE_JS_FUNCTION(incrementTxnNumber);
    };

    static const JSFunctionSpec methods[8];

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(SessionInfoSlotCount) | BaseInfo::finalizeFlag;
    static const InstallType installType = InstallType::Private;

    static void make(JSContext* cx,
                     JS::MutableHandleObject obj,
                     std::shared_ptr<DBClientBase> client,
                     BSONObj lsid);
};

}  // namespace mozjs
}  // namespace mongo
