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

#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/RootingAPI.h>

#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/cursor.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"  // IWYU pragma: keep

namespace mongo {
namespace mozjs {

const JSFunctionSpec CursorInfo::methods[9] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(close, CursorInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(hasNext, CursorInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(next, CursorInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(objsLeftInBatch, CursorInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getId, CursorInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(readOnly, CursorInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(isClosed, CursorInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(hasMoreToCome, CursorInfo),
    JS_FS_END,
};

const char* const CursorInfo::className = "Cursor";

namespace {

DBClientCursor* getCursor(JSObject* thisv) {
    auto cursorHolder = JS::GetMaybePtrFromReservedSlot<CursorInfo::CursorHolder>(
        thisv, CursorInfo::CursorHolderSlot);
    return cursorHolder ? cursorHolder->cursor.get() : nullptr;
}

DBClientCursor* getCursor(JS::CallArgs& args) {
    return getCursor(args.thisv().toObjectOrNull());
}

}  // namespace

void CursorInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto cursor = JS::GetMaybePtrFromReservedSlot<CursorInfo::CursorHolder>(obj, CursorHolderSlot);

    if (cursor) {
        getScope(gcCtx)->trackedDelete(cursor);
    }
}

void CursorInfo::Functions::next::call(JSContext* cx, JS::CallArgs args) {
    auto cursor = getCursor(args);

    if (!cursor) {
        args.rval().setUndefined();
        return;
    }

    uassert(9279715, "Cursor is not initialized", cursor->isInitialized());

    ObjectWrapper o(cx, args.thisv());

    BSONObj bson = cursor->next();
    bool ro = o.hasField(InternedString::_ro) ? o.getBoolean(InternedString::_ro) : false;

    // getOwned because cursor->next() gives us unowned bson from an internal
    // buffer and we need to make a copy
    ValueReader(cx, args.rval()).fromBSON(bson.getOwned(), nullptr, ro);
}

void CursorInfo::Functions::hasNext::call(JSContext* cx, JS::CallArgs args) {
    auto cursor = getCursor(args);

    if (!cursor) {
        args.rval().setBoolean(false);
        return;
    }

    uassert(9279716, "Cursor is not initialized", cursor->isInitialized());

    args.rval().setBoolean(cursor->more());
}

void CursorInfo::Functions::objsLeftInBatch::call(JSContext* cx, JS::CallArgs args) {
    auto cursor = getCursor(args);

    if (!cursor) {
        args.rval().setInt32(0);
        return;
    }

    uassert(9279717, "Cursor is not initialized", cursor->isInitialized());

    args.rval().setInt32(cursor->objsLeftInBatch());
}

void CursorInfo::Functions::readOnly::call(JSContext* cx, JS::CallArgs args) {
    ObjectWrapper(cx, args.thisv()).setBoolean(InternedString::_ro, true);

    args.rval().set(args.thisv());
}

void CursorInfo::Functions::getId::call(JSContext* cx, JS::CallArgs args) {
    auto cursor = getCursor(args);

    if (!cursor) {
        ValueReader(cx, args.rval()).fromInt64(0);
        return;
    }

    ValueReader(cx, args.rval()).fromInt64(cursor->getCursorId());
}

void CursorInfo::Functions::close::call(JSContext* cx, JS::CallArgs args) {
    auto cursor = getCursor(args);

    if (cursor)
        cursor->kill();

    args.rval().setUndefined();
}

void CursorInfo::Functions::isClosed::call(JSContext* cx, JS::CallArgs args) {
    auto cursor = getCursor(args);

    if (!cursor) {
        args.rval().setBoolean(true);
        return;
    }

    args.rval().setBoolean(cursor->isDead());
}

void CursorInfo::Functions::hasMoreToCome::call(JSContext* cx, JS::CallArgs args) {
    auto cursor = getCursor(args);

    if (!cursor) {
        args.rval().setBoolean(false);
        return;
    }

    uassert(9279718, "Cursor is not initialized", cursor->isInitialized());

    args.rval().setBoolean(cursor->hasMoreToCome());
}

}  // namespace mozjs
}  // namespace mongo
