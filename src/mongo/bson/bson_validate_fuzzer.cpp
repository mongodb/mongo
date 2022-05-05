/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bson_validate_old.h"
#include "mongo/logv2/log.h"
#include "mongo/util/hex.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    using namespace mongo;
    // The fuzzerOnly version is an older, slower implementation with less precise error messages.
    // If we rewrite in the future we can use the current implementation as the fuzzerOnly one.
    Status ret = validateBSON(Data, Size);
    Status oldRet = fuzzerOnly::validateBSON(Data, Size);

    // The implementations differ a bit in error codes, but that's OK.
    if (oldRet.code() != ret.code()) {
        LOGV2_DEBUG(4496700,
                    2,
                    "validateBSON return code has changed",
                    "input"_attr = hexblob::encode(Data, Size),
                    "ret"_attr = ret,
                    "oldRet"_attr = oldRet);
        // We trust the old implementation's OK, so dump the object to hopefully give a hint.
        if (oldRet.isOK())
            LOGV2(4496701, "Object used to be valid", "obj"_attr = BSONObj(Data));
    }

    // This will effectively cause the fuzer to find differences between both implementations
    // (as they'd lead to crashes), while using edge cases leading to interesting control flow
    // paths in both implementations.
    invariant(oldRet.isOK() == ret.isOK());
    return 0;
}
