// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/common/hex_md5.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <string_view>

namespace mongo {

static BSONObj native_tostrictjson(const mongo::BSONObj& args, void* data) {
    uassert(40275,
            "tostrictjson takes a single BSON object argument, and on optional boolean argument "
            "for prettyPrint -- tostrictjson(obj, prettyPrint = false)",
            args.nFields() >= 1 && args.firstElement().isABSONObj() &&
                (args.nFields() == 1 || (args.nFields() == 2 && args["1"].isBoolean())));

    bool prettyPrint = false;
    if (args.nFields() == 2) {
        prettyPrint = args["1"].boolean();
    }
    return BSON("" << tojson(args.firstElement().embeddedObject(), LegacyStrict, prettyPrint));
}

// ---------------------------------
// ---- installer           --------
// ---------------------------------

void installGlobalUtils(Scope& scope) {
    scope.injectNative("hex_md5", native_hex_md5);
    scope.injectNative("tostrictjson", native_tostrictjson);
}

}  // namespace mongo
