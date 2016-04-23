/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/db.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/local_sharding_info.h"
#include "mongo/scripting/mozjs/idwrapper.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"

namespace mongo {
namespace mozjs {

const char* const DBInfo::className = "DB";

void DBInfo::getProperty(JSContext* cx,
                         JS::HandleObject obj,
                         JS::HandleId id,
                         JS::MutableHandleValue vp) {
    // 2nd look into real values, may be cached collection object
    if (!vp.isUndefined()) {
        auto scope = getScope(cx);
        auto opContext = scope->getOpContext();

        if (opContext && vp.isObject()) {
            ObjectWrapper o(cx, vp);

            if (o.hasOwnField(InternedString::_fullName)) {
                // need to check every time that the collection did not get sharded
                if (haveLocalShardingInfo(opContext, o.getString(InternedString::_fullName)))
                    uasserted(ErrorCodes::BadValue, "can't use sharded collection from db.eval");
            }
        }

        return;
    }

    JS::RootedObject parent(cx);
    if (!JS_GetPrototype(cx, obj, &parent))
        uasserted(ErrorCodes::JSInterpreterFailure, "Couldn't get prototype");

    ObjectWrapper parentWrapper(cx, parent);

    if (parentWrapper.hasOwnField(id)) {
        parentWrapper.getValue(id, vp);
        return;
    }

    IdWrapper idw(cx, id);

    // if starts with '_' we dont return collection, one must use getCollection()
    if (idw.isString()) {
        JSStringWrapper jsstr;
        auto sname = idw.toStringData(&jsstr);
        if (sname.size() == 0 || sname[0] == '_') {
            return;
        }
    }

    // no hit, create new collection
    JS::RootedValue getCollection(cx);
    parentWrapper.getValue(InternedString::getCollection, &getCollection);

    if (!(getCollection.isObject() && JS_ObjectIsFunction(cx, getCollection.toObjectOrNull()))) {
        uasserted(ErrorCodes::BadValue, "getCollection is not a function");
    }

    JS::AutoValueArray<1> args(cx);

    idw.toValue(args[0]);

    JS::RootedValue coll(cx);
    ObjectWrapper(cx, obj).callMethod(getCollection, args, &coll);

    uassert(16861,
            "getCollection returned something other than a collection",
            getScope(cx)->getProto<DBCollectionInfo>().instanceOf(coll));

    // cache collection for reuse, don't enumerate
    ObjectWrapper(cx, obj).defineProperty(id, coll, 0);

    vp.set(coll);
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

    if (!NamespaceString::validDBName(dbName, NamespaceString::DollarInDbNameBehavior::Allow))
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "[" << dbName << "] is not a valid database name");

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
