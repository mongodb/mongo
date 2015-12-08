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

#include "mongo/scripting/mozjs/dbref.h"

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"

namespace mongo {
namespace mozjs {

const char* const DBRefInfo::className = "DBRef";

void DBRefInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (!(args.length() == 2 || args.length() == 3))
        uasserted(ErrorCodes::BadValue, "DBRef needs 2 or 3 arguments");

    if (!args.get(0).isString())
        uasserted(ErrorCodes::BadValue, "DBRef 1st parameter must be a string");

    JS::RootedObject thisv(cx);
    scope->getProto<DBRefInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    o.setValue(InternedString::dollar_ref, args.get(0));
    o.setValue(InternedString::dollar_id, args.get(1));

    if (args.length() == 3) {
        if (!args.get(2).isString())
            uasserted(ErrorCodes::BadValue, "DBRef 3rd parameter must be a string");

        o.setValue(InternedString::dollar_db, args.get(2));
    }

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
