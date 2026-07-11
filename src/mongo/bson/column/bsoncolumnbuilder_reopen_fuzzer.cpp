// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bson_validate.h"
#include "mongo/bson/column/bsoncolumn_fuzzer_impl.h"

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    using namespace mongo;

    // Structural validation so data is memory safe to parse
    if (!validateBSONColumn(Data, Size).isOK()) {
        return 0;
    }

    // Perform fuzzing, will invariant internally if any issue is found.
    bsoncolumn::fuzzer(Data, Size);
    return 0;
}
