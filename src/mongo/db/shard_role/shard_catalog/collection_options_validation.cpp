// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_options_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/str.h"

namespace mongo::collection_options_validation {
Status validateStorageEngineOptions(const BSONObj& storageEngine) {
    // Every field inside 'storageEngine' must be a document.
    // Format:
    // {
    //     ...
    //     storageEngine: {
    //         storageEngine1: {
    //             ...
    //         },
    //         storageEngine2: {
    //             ...
    //         }
    //     },
    //     ...
    // }
    for (auto&& elem : storageEngine) {
        if (elem.type() != BSONType::object) {
            return {ErrorCodes::BadValue,
                    str::stream() << "'storageEngine." << elem.fieldName()
                                  << "' must be an embedded document"};
        }
    }
    return Status::OK();
}

}  // namespace mongo::collection_options_validation
