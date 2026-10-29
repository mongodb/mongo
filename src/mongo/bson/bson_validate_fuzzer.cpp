// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
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
    //
    // Ignore changes due to column validation failing additional entries
    invariant(oldRet.isOK() == ret.isOK() || ret.code() == ErrorCodes::NonConformantBSON);
    return 0;
}
