
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

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/dbref.h"

#include "mongo/scripting/mozjs/bson.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuewriter.h"

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

void DBRefInfo::finalize(js::FreeOp* fop, JSObject* obj) {
    BSONInfo::finalize(fop, obj);
}

void DBRefInfo::enumerate(JSContext* cx,
                          JS::HandleObject obj,
                          JS::AutoIdVector& properties,
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

    auto scope = getScope(cx);

    scope->getProto<DBRefInfo>().newObject(obj);
    JS_SetPrivate(obj, JS_GetPrivate(local));
    JS_SetPrivate(local, nullptr);
}

}  // namespace mozjs
}  // namespace mongo
