/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/column/bsoncolumn_fuzzer_impl.h"

#include "mongo/base/status.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"

namespace mongo::bsoncolumn {
void fuzzer(const char* binary, size_t size) {

    Status iteratorDecompressorResult = Status::OK();
    Status binaryReopenResult = Status::OK();

    // Decompress all and append data into a reference builder.
    BSONColumnBuilder reference;
    try {
        BSONColumn column(binary, size);
        for (auto&& val : column) {
            reference.append(val);
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
