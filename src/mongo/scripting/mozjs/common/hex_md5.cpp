// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/hex_md5.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/md5.h"

namespace mongo {

BSONObj native_hex_md5(const BSONObj& args, void* data) {
    uassert(10261,
            "hex_md5 takes a single string argument -- hex_md5(string)",
            args.nFields() == 1 && args.firstElement().type() == BSONType::string);
    std::string_view sd = args.firstElement().valueStringDataSafe();

    md5digest d;
    md5_state_t st;
    md5_init_state_deprecated(&st);
    md5_append_deprecated(&st, reinterpret_cast<const md5_byte_t*>(sd.data()), sd.size());
    md5_finish_deprecated(&st, d);

    return BSON("" << digestToString(d));
}

}  // namespace mongo
