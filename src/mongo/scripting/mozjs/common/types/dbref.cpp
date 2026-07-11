// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/dbref.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/types/bson.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <jsapi.h>

#include <js/CallArgs.h>
#include <js/Class.h>
#include <js/Object.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const char* const DBRefInfo::className = "DBRef";

void DBRefInfo::construct(JSContext* cx, JS::CallArgs args) {
    if (!(args.length() == 2 || args.length() == 3))
        uasserted(ErrorCodes::BadValue, "DBRef needs 2 or 3 arguments");

    if (!args.get(0).isString())
        uasserted(ErrorCodes::BadValue, "DBRef 1st parameter must be a string");

    JS::RootedObject thisv(cx, JS_NewPlainObject(cx));
    ObjectWrapper o(cx, thisv);

    o.setValue(InternedString::dollar_ref, args.get(0));
    o.setValue(InternedString::dollar_id, args.get(1));

    if (args.length() == 3) {
        if (!args.get(2).isString())
            uasserted(ErrorCodes::BadValue, "DBRef 3rd parameter must be a string");

        o.setValue(InternedString::dollar_db, args.get(2));
    }

    JS::RootedObject out(cx);
    DBRefInfo::make(cx, &out, o.toBSON(), nullptr, false);

    args.rval().setObjectOrNull(out);
}

void DBRefInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    BSONInfo::finalize(gcCtx, obj);
}

void DBRefInfo::enumerate(JSContext* cx,
                          JS::HandleObject obj,
                          JS::MutableHandleIdVector properties,
                          bool enumerableOnly) {
    BSONInfo::enumerate(cx, obj, properties, enumerableOnly);
}

void DBRefInfo::setProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::HandleValue vp,
                            JS::HandleValue receiver,
                            JS::ObjectOpResult& result) {
    BSONInfo::setProperty(cx, obj, id, vp, receiver, result);
}

void DBRefInfo::delProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::ObjectOpResult& result) {
    BSONInfo::delProperty(cx, obj, id, result);
}

void DBRefInfo::resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp) {
    BSONInfo::resolve(cx, obj, id, resolvedp);
}

void DBRefInfo::make(
    JSContext* cx, JS::MutableHandleObject obj, BSONObj bson, const BSONObj* parent, bool ro) {

    JS::RootedObject local(cx);
    BSONInfo::make(cx, &local, std::move(bson), parent, ro);

    auto* runtime = getCommonRuntime(cx);

    getProto<DBRefInfo>(runtime).newObject(obj);

    JS::SetReservedSlot(
        obj,
        BSONHolderSlot,
        JS::PrivateValue(JS::GetMaybePtrFromReservedSlot<void>(local, BSONInfo::BSONHolderSlot)));
    JS::SetReservedSlot(local, BSONInfo::BSONHolderSlot, JS::UndefinedValue());
}

}  // namespace mozjs
}  // namespace mongo
