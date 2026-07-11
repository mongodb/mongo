// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/object.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/util/assert_util.h"

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const JSFunctionSpec ObjectInfo::methods[2] = {
    MONGO_ATTACH_JS_FUNCTION(bsonsize),
    JS_FS_END,
};

const char* const ObjectInfo::className = "Object";

void ObjectInfo::Functions::bsonsize::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1)
        uasserted(ErrorCodes::BadValue, "bsonsize needs 1 argument");

    if (args.get(0).isNull()) {
        args.rval().setInt32(0);
        return;
    }

    if (!args.get(0).isObject())
        uasserted(ErrorCodes::BadValue, "argument to bsonsize has to be an object");

    args.rval().setInt32(ValueWriter(cx, args.get(0)).toBSON().objsize());
}

}  // namespace mozjs
}  // namespace mongo
