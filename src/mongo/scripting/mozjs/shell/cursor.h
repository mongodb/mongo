// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/dbclient_cursor.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/common/base.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Wraps a DBClientCursor in javascript
 *
 * Note that the install is private, so this class should only be constructible
 * from C++. Current callers are all via the Mongo object.
 */
struct CursorInfo : public BaseInfo {
    enum Slots { CursorHolderSlot, CursorInfoSlotCount };

    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(close);
        MONGO_DECLARE_JS_FUNCTION(hasNext);
        MONGO_DECLARE_JS_FUNCTION(isClosed);
        MONGO_DECLARE_JS_FUNCTION(next);
        MONGO_DECLARE_JS_FUNCTION(objsLeftInBatch);
        MONGO_DECLARE_JS_FUNCTION(readOnly);
        MONGO_DECLARE_JS_FUNCTION(getId);
        MONGO_DECLARE_JS_FUNCTION(hasMoreToCome);
    };

    static const JSFunctionSpec methods[9];

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(CursorInfoSlotCount) | BaseInfo::finalizeFlag;
    static const InstallType installType = InstallType::Private;

    /**
     * We need this because the DBClientBase can go out of scope before all of
     * its children (as in global shutdown). So we have to manage object
     * lifetimes in C++ land.
     */
    struct CursorHolder {
        CursorHolder(std::unique_ptr<DBClientCursor> cursor, std::shared_ptr<DBClientBase> client)
            : client(std::move(client)), cursor(std::move(cursor)) {}

        std::shared_ptr<DBClientBase> client;
        std::unique_ptr<DBClientCursor> cursor;
    };
};

}  // namespace mozjs
}  // namespace mongo
