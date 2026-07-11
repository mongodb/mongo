// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/types/regexp.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep

#include <string>

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const JSFunctionSpec RegExpInfo::methods[2] = {
    MONGO_ATTACH_JS_FUNCTION(toJSON),
    JS_FS_END,
};

const char* const RegExpInfo::className = "RegExp";

void RegExpInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    ObjectWrapper o(cx, args.thisv());

    auto regex_string = o.getString(InternedString::source);
    auto flags_string = o.getString(InternedString::flags);

    ValueReader(cx, args.rval())
        .fromBSON(BSON("$regex" << regex_string << "$options" << flags_string), nullptr, false);
}

}  // namespace mozjs
}  // namespace mongo
