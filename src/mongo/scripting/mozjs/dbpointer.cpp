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

#include "mongo/scripting/mozjs/dbpointer.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/oid.h"
#include "mongo/scripting/mozjs/wraptype.h"
#include "mongo/util/assert_util.h"

#include <js/CallArgs.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const char* const DBPointerInfo::className = "DBPointer";

void DBPointerInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (args.length() != 2)
        uasserted(ErrorCodes::BadValue, "DBPointer needs 2 arguments");

    if (!args.get(0).isString())
        uasserted(ErrorCodes::BadValue, "DBPointer 1st parameter must be a string");

    if (!scope->getProto<OIDInfo>().instanceOf(args.get(1)))
        uasserted(ErrorCodes::BadValue, "DBPointer 2nd parameter must be an ObjectId");

    JS::RootedObject thisv(cx);
    scope->getProto<DBPointerInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    o.setValue(InternedString::ns, args.get(0));
    o.setValue(InternedString::id, args.get(1));

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
