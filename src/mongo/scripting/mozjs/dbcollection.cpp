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

#include "mongo/scripting/mozjs/dbcollection.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/local_sharding_info.h"
#include "mongo/scripting/mozjs/bson.h"
#include "mongo/scripting/mozjs/db.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuewriter.h"

namespace mongo {
namespace mozjs {

const char* const DBCollectionInfo::className = "DBCollection";

void DBCollectionInfo::getProperty(JSContext* cx,
                                   JS::HandleObject obj,
                                   JS::HandleId id,
                                   JS::MutableHandleValue vp) {
    DBInfo::getProperty(cx, obj, id, vp);
}

void DBCollectionInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (args.length() != 4)
        uasserted(ErrorCodes::BadValue, "collection constructor requires 4 arguments");

    for (unsigned i = 0; i < args.length(); ++i) {
        uassert(ErrorCodes::BadValue,
                "collection constructor called with undefined argument",
                !args.get(i).isUndefined());
    }

    JS::RootedObject thisv(cx);
    scope->getProto<DBCollectionInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    o.setValue(InternedString::_mongo, args.get(0));
    o.setValue(InternedString::_db, args.get(1));
    o.setValue(InternedString::_shortName, args.get(2));
    o.setValue(InternedString::_fullName, args.get(3));

    std::string fullName = ValueWriter(cx, args.get(3)).toString();

    auto context = scope->getOpContext();
    if (context && haveLocalShardingInfo(context, fullName)) {
        uasserted(ErrorCodes::BadValue, "can't use sharded collection from db.eval");
    }

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
