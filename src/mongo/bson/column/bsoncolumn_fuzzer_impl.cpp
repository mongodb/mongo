// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/column/bsoncolumn_fuzzer_impl.h"

#include "mongo/base/status.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/column/bsoncolumn_expressions.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"

namespace mongo::bsoncolumn {
void fuzzer(const char* binary, size_t size) {

    Status iteratorDecompressorResult = Status::OK();
    Status binaryReopenResult = Status::OK();

    // Decompress all and append data into a reference builder.
    BSONColumnBuilder reference;
    size_t cnt = 0;
    try {
        BSONColumn column(binary, size);
        for (auto&& val : column) {
            reference.append(val);
            ++cnt;
        }

    } catch (const DBException& ex) {
        iteratorDecompressorResult = ex.toStatus();
    }

    // Reopen BSONColumnBuilder with binary
    BSONColumnBuilder<> reopen = [&]() {
        try {
            return BSONColumnBuilder<>(binary, size);
        } catch (const DBException& ex) {
            binaryReopenResult = ex.toStatus();
            return BSONColumnBuilder<>();
        }
    }();

    // Compare error handling between the two implementations
    if (iteratorDecompressorResult.isOK() != binaryReopenResult.isOK()) {
        std::string err = "decompression: " + iteratorDecompressorResult.toString() +
            ", reopen: " + binaryReopenResult.toString();
        invariant(false, err);
    }

    // Verify bsoncolumn::count
    if (iteratorDecompressorResult.isOK()) {
        invariant(bsoncolumn::count(binary, size) == cnt);
    }

    auto diff = reference.intermediate();
    // Verify that this is binary that would be produced by our BSONColumnBuilder
    if ((size_t)diff.size() != size || memcmp(binary, diff.data(), size) != 0) {
        return;
    }

    // Check that the internal state is identical with a builder that has appended all data and
    // called intermediate()
    invariant(reopen.isInternalStateIdentical(reference));
}
}  // namespace mongo::bsoncolumn
