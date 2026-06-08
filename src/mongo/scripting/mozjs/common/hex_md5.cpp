/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/scripting/mozjs/common/hex_md5.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/md5.h"

namespace mongo {

BSONObj native_hex_md5(const BSONObj& args, void* data) {
    uassert(10261,
            "hex_md5 takes a single string argument -- hex_md5(string)",
            args.nFields() == 1 && args.firstElement().type() == BSONType::string);
    StringData sd = args.firstElement().valueStringDataSafe();

    md5digest d;
    md5_state_t st;
    md5_init_state_deprecated(&st);
    md5_append_deprecated(&st, reinterpret_cast<const md5_byte_t*>(sd.data()), sd.size());
    md5_finish_deprecated(&st, d);

    return BSON("" << digestToString(d));
}

}  // namespace mongo
