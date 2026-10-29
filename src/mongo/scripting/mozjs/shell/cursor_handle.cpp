// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/scripting/mozjs/shell/cursor_handle.h"

#include "mongo/client/dbclient_base.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/scripting/mozjs/shell/scripting_util_gen.h"
#include "mongo/util/assert_util.h"

#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace mozjs {

const JSFunctionSpec CursorHandleInfo::methods[2] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(zeroCursorId, CursorHandleInfo),
    JS_FS_END,
};

const char* const CursorHandleInfo::className = "CursorHandle";

namespace {

long long* getCursorId(JSObject* thisv) {
    CursorHandleInfo::CursorTracker* tracker =
        JS::GetMaybePtrFromReservedSlot<CursorHandleInfo::CursorTracker>(
            thisv, CursorHandleInfo::CursorTrackerSlot);
    if (tracker) {
        return &tracker->cursorId;
    }

    return nullptr;
}

long long* getCursorId(JS::CallArgs& args) {
    return getCursorId(args.thisv().toObjectOrNull());
}

}  // namespace

void CursorHandleInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto cursorTracker =
        JS::GetMaybePtrFromReservedSlot<CursorHandleInfo::CursorTracker>(obj, CursorTrackerSlot);
    if (cursorTracker) {
        const long long cursorId = cursorTracker->cursorId;
        if (!skipShellCursorFinalize && cursorId) {
            try {
                cursorTracker->client->killCursor(cursorTracker->ns, cursorId);
            } catch (...) {
                auto status = exceptionToStatus();

                try {
                    LOGV2_INFO(22782,
                               "Failed to kill cursor",
                               "cursorId"_attr = cursorId,
                               "error"_attr = redact(status));
                } catch (...) {
                    // This is here in case logging fails.
                }
            }
        }

        getScope(gcCtx)->trackedDelete(cursorTracker);
    }
}

void CursorHandleInfo::Functions::zeroCursorId::call(JSContext* cx, JS::CallArgs args) {
    long long* cursorId = getCursorId(args);
    if (cursorId) {
        *cursorId = 0;
    }
}

}  // namespace mozjs
}  // namespace mongo
