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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/md5.h"

#include <string>

namespace mongo {

static BSONObj native_hex_md5(const BSONObj& args, void* data) {
    uassert(10261,
            "hex_md5 takes a single string argument -- hex_md5(string)",
            args.nFields() == 1 && args.firstElement().type() == BSONType::string);
    StringData sd = args.firstElement().valueStringDataSafe();

    md5digest d;
    md5_state_t st;
    md5_init_state(&st);
    md5_append(&st, reinterpret_cast<const md5_byte_t*>(sd.data()), sd.size());
    md5_finish(&st, d);

    return BSON("" << digestToString(d));
}

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
