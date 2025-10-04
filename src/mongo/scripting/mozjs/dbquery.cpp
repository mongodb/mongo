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

#include "mongo/scripting/mozjs/dbquery.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/idwrapper.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/wraptype.h"
#include "mongo/util/assert_util.h"

#include <jsapi.h>
#include <jsfriendapi.h>

#include <js/CallArgs.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>
#include <js/ValueArray.h>

namespace mongo {
namespace mozjs {

const char* const DBQueryInfo::className = "DBQuery";

void DBQueryInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (args.length() < 4)
        uasserted(ErrorCodes::BadValue, "dbQuery constructor requires at least 4 arguments");

    JS::RootedObject thisv(cx);
    scope->getProto<DBQueryInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    o.setValue(InternedString::_mongo, args.get(0));
    o.setValue(InternedString::_db, args.get(1));
    o.setValue(InternedString::_collection, args.get(2));
    o.setValue(InternedString::_ns, args.get(3));

    JSObject* newPlainObject = JS_NewPlainObject(cx);
    uassert(6887104, "failed to create new JS object", newPlainObject);
    JS::RootedObject emptyObj{cx, newPlainObject};
    JS::RootedValue emptyObjVal(cx);
    emptyObjVal.setObjectOrNull(emptyObj);

    JS::RootedValue nullVal(cx);
    nullVal.setNull();

    if (args.length() > 4 && args.get(4).isObject()) {
        o.setValue(InternedString::_filter, args.get(4));
    } else {
        o.setValue(InternedString::_filter, nullVal);
    }

    if (args.length() > 5 && args.get(5).isObject()) {
        o.setValue(InternedString::_projection, args.get(5));
    } else {
        o.setValue(InternedString::_projection, nullVal);
    }

    if (args.length() > 6 && args.get(6).isNumber()) {
        o.setValue(InternedString::_limit, args.get(6));
    } else {
        o.setNumber(InternedString::_limit, 0);
    }

    if (args.length() > 7 && args.get(7).isNumber()) {
        o.setValue(InternedString::_skip, args.get(7));
    } else {
        o.setNumber(InternedString::_skip, 0);
    }

    if (args.length() > 8 && args.get(8).isNumber()) {
        o.setValue(InternedString::_batchSize, args.get(8));
    } else {
        o.setNumber(InternedString::_batchSize, 0);
    }

    if (args.length() > 9 && args.get(9).isNumber()) {
        o.setValue(InternedString::_options, args.get(9));
    } else {
        o.setNumber(InternedString::_options, 0);
    }

    o.setValue(InternedString::_additionalCmdParams, emptyObjVal);

    o.setValue(InternedString::_cursor, nullVal);
    o.setNumber(InternedString::_numReturned, 0);

    args.rval().setObjectOrNull(thisv);
}

void DBQueryInfo::resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp) {
    *resolvedp = false;

    IdWrapper wid(cx, id);

    // We only use this for index access
    if (!wid.isInt()) {
        return;
    }

    JS::RootedObject parent(cx);
    if (!JS_GetPrototype(cx, obj, &parent))
        uasserted(ErrorCodes::InternalError, "Couldn't get prototype");

    ObjectWrapper parentWrapper(cx, parent);

    JS::RootedValue arrayAccess(cx);
    parentWrapper.getValue(InternedString::arrayAccess, &arrayAccess);

    if (arrayAccess.isObject() && js::IsFunctionObject(arrayAccess.toObjectOrNull())) {
        JS::RootedValueArray<1> args(cx);

        args[0].setInt32(wid.toInt32());

        JS::RootedValue vp(cx);

        ObjectWrapper(cx, obj).callMethod(arrayAccess, args, &vp);

        if (!vp.isNullOrUndefined()) {
            ObjectWrapper o(cx, obj);

            // Assumes the user won't modify the contents of what DBQuery::arrayAccess returns
            // otherwise we need to install a getter.
            o.defineProperty(id, vp, 0);
        }

        *resolvedp = true;
    }
}

}  // namespace mozjs
}  // namespace mongo
