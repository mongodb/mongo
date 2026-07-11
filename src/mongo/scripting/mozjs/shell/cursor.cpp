// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/cursor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/scripting/mozjs/shell/implscope.h"

#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

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
