// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/dbquery.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/common/idwrapper.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/scripting/mozjs/shell/implscope.h"
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
