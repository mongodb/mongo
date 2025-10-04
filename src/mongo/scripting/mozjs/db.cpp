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

#include "mongo/scripting/mozjs/db.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/scripting/mozjs/dbcollection.h"
#include "mongo/scripting/mozjs/idwrapper.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wraptype.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <jsapi.h>
#include <jsfriendapi.h>

#include <js/CallArgs.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>
#include <js/ValueArray.h>

namespace mongo {
namespace mozjs {

const char* const DBInfo::className = "DB";

void DBInfo::resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp) {
    *resolvedp = false;

    JS::RootedValue coll(cx);

    JS::RootedObject parent(cx);
    if (!JS_GetPrototype(cx, obj, &parent))
        uasserted(ErrorCodes::JSInterpreterFailure, "Couldn't get prototype");

    ObjectWrapper parentWrapper(cx, parent);
    ObjectWrapper o(cx, obj);

    IdWrapper idw(cx, id);

    // if starts with '_' we dont return collection, one must use getCollection()
    if (idw.isString()) {
        JSStringWrapper jsstr;
        auto sname = idw.toStringData(&jsstr);
        if (sname.size() == 0 || sname[0] == '_') {
            return;
        }

        // SpiderMonkey will call resolve even for __proto__ so acknowledge it exists.
        if (sname == "__proto__"_sd) {
            *resolvedp = true;
            return;
        }
    }

    // Check if this exists on the parent, ie. DBCollection::resolve case
    if (parentWrapper.alreadyHasOwnField(id)) {
        parentWrapper.getValue(id, &coll);

        o.defineProperty(id, coll, 0);

        *resolvedp = true;
        return;
    }

    // no hit, create new collection
    JS::RootedValue getCollection(cx);
    parentWrapper.getValue(InternedString::getCollection, &getCollection);

    // Check if getCollection has been installed yet
    // It is undefined if the user has a db name the same as one of methods/properties of the DB
    // object.
    if (!(getCollection.isObject() && js::IsFunctionObject(getCollection.toObjectOrNull()))) {
        return;
    }

    JS::RootedValueArray<1> args(cx);

    idw.toValue(args[0]);

    ObjectWrapper(cx, obj).callMethod(getCollection, args, &coll);

    uassert(16861,
            "getCollection returned something other than a collection",
            getScope(cx)->getProto<DBCollectionInfo>().instanceOf(coll));

    // cache collection for reuse, don't enumerate
    ObjectWrapper(cx, obj).defineProperty(id, coll, 0);

    *resolvedp = true;
}

void DBInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (args.length() != 2)
        uasserted(ErrorCodes::BadValue, "db constructor requires 2 arguments");

    for (unsigned i = 0; i < args.length(); ++i) {
        uassert(ErrorCodes::BadValue,
                "db initializer called with undefined argument",
                !args.get(i).isUndefined());
    }

    JS::RootedObject thisv(cx);
    scope->getProto<DBInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    o.setValue(InternedString::_mongo, args.get(0));
    o.setValue(InternedString::_name, args.get(1));

    std::string dbName = ValueWriter(cx, args.get(1)).toString();

    if (!DatabaseName::validDBName(dbName, DatabaseName::DollarInDbNameBehavior::Allow))
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "[" << dbName << "] is not a valid database name");

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
