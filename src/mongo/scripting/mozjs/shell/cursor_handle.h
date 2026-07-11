// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/namespace_string.h"
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
 * Wraps a DBClientCursor in javascript.
 *
 * Note that the install is private, so this class should only be constructible from C++. Current
 * callers are all via the Mongo object.
 */
struct CursorHandleInfo : public BaseInfo {
    enum Slots { CursorTrackerSlot, CursorHandleInfoSlotCount };

    static void finalize(JS::GCContext* gcCtx, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(zeroCursorId);
    };

    static const JSFunctionSpec methods[2];

    static const char* const className;
    static const unsigned classFlags =
        JSCLASS_HAS_RESERVED_SLOTS(CursorHandleInfoSlotCount) | BaseInfo::finalizeFlag;
    static const InstallType installType = InstallType::Private;

    /**
     * We need this because the DBClientBase can go out of scope before all of its children (as
     * in global shutdown). So we have to manage object lifetimes in C++ land.
     */
    struct CursorTracker {
        CursorTracker(NamespaceString ns, long long curId, std::shared_ptr<DBClientBase> client)
            : client(std::move(client)), ns(std::move(ns)), cursorId(curId) {}

        std::shared_ptr<DBClientBase> client;
        NamespaceString ns;
        long long cursorId;
    };
};

}  // namespace mozjs
}  // namespace mongo
