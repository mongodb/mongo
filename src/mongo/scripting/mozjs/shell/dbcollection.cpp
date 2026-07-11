// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/dbcollection.h"

#include "mongo/base/error_codes.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/scripting/mozjs/shell/db.h"
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/util/assert_util.h"

#include <js/CallArgs.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const char* const DBCollectionInfo::className = "DBCollection";

void DBCollectionInfo::resolve(JSContext* cx,
                               JS::HandleObject obj,
                               JS::HandleId id,
                               bool* resolvedp) {
    DBInfo::resolve(cx, obj, id, resolvedp);
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

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
