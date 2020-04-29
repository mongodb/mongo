/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_base.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/mozjs/cursor_handle.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/scripting_util_gen.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"

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
        static_cast<CursorHandleInfo::CursorTracker*>(JS_GetPrivate(thisv));
    if (tracker) {
        return &tracker->cursorId;
    }

    return nullptr;
}

long long* getCursorId(JS::CallArgs& args) {
    return getCursorId(args.thisv().toObjectOrNull());
}

}  // namespace

void CursorHandleInfo::finalize(js::FreeOp* fop, JSObject* obj) {
    auto cursorTracker = static_cast<CursorHandleInfo::CursorTracker*>(JS_GetPrivate(obj));
    if (cursorTracker) {
        const long long cursorId = cursorTracker->cursorId;
        if (!skipShellCursorFinalize && cursorId) {
            try {
                cursorTracker->client->killCursor(cursorTracker->ns, cursorId);
            } catch (...) {
                auto status = exceptionToStatus();

                try {
                    LOGV2_INFO(22782,
                               "Failed to kill cursor {cursorId} due to {status}",
                               "cursorId"_attr = cursorId,
                               "status"_attr = status);
                } catch (...) {
                    // This is here in case logging fails.
                }
            }
        }

        getScope(fop)->trackedDelete(cursorTracker);
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
