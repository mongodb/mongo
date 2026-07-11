// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

// Any data that passes validation should not have structural issues (i.e. cause memory
// faults), we do not currently validate full correctness, so decoding is allowed to fail
extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    using namespace mongo;

    // Skip inputs that do not pass validation
    if (!validateBSONColumn(Data, Size).isOK()) {
        return 0;
    }

    try {
        BSONColumn(Data, Size).size();
    } catch (...) {
    }

    return 0;
}
