// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/dbpointer.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/types/oid.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/util/assert_util.h"

#include <js/CallArgs.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const char* const DBPointerInfo::className = "DBPointer";

void DBPointerInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto* runtime = getCommonRuntime(cx);

    if (args.length() != 2)
        uasserted(ErrorCodes::BadValue, "DBPointer needs 2 arguments");

    if (!args.get(0).isString())
        uasserted(ErrorCodes::BadValue, "DBPointer 1st parameter must be a string");

    if (!getProto<OIDInfo>(runtime).instanceOf(args.get(1)))
        uasserted(ErrorCodes::BadValue, "DBPointer 2nd parameter must be an ObjectId");

    JS::RootedObject thisv(cx);
    getProto<DBPointerInfo>(runtime).newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    o.setValue(InternedString::ns, args.get(0));
    o.setValue(InternedString::id, args.get(1));

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
