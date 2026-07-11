// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/tailable_mode.h"

#include "mongo/base/error_codes.h"


namespace mongo {

StatusWith<TailableModeEnum> tailableModeFromBools(bool isTailable, bool isAwaitData) {
    if (isTailable) {
        if (isAwaitData) {
            return TailableModeEnum::kTailableAndAwaitData;
        }
        return TailableModeEnum::kTailable;
    } else if (isAwaitData) {
        return {ErrorCodes::FailedToParse,
                "Cannot set 'awaitData' without also setting 'tailable'"};
    }
    return TailableModeEnum::kNormal;
}

}  // namespace mongo
