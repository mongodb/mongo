// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_parsing_validators.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/str.h"

namespace mongo {

Status validateObjectIsEmpty(const BSONObj& object) {
    if (!object.isEmpty()) {
        return {
            ErrorCodes::Error{31170},
            str::stream() << "expected an empty object, but got " << object,
        };
    }
    return Status::OK();
}
}  // namespace mongo
