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

#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/util/bsonobj_traversal.h"
#include "mongo/util/base64.h"

// Returns true if the binary contains interleaved data. This function just scans the binary for an
// interleaved start control byte, it does no validation nor decompression.
static bool isDataInterleaved(const char* binary, size_t size) {
    using namespace mongo;
    const char* pos = binary;
    const char* end = binary + size;

    while (pos != end) {
        uint8_t control = *pos;
        if (control == stdx::to_underlying(BSONType::eoo)) {
            // Reached the end of the binary.
            return false;
        }

        if (bsoncolumn::isInterleavedStartControlByte(control)) {
            return true;
        }

        if (bsoncolumn::isUncompressedLiteralControlByte(control)) {
            // Scan over the entire literal.
            BSONElement literal(pos, 1, BSONElement::TrustedInitTag{});
            pos += literal.size();
            continue;
        }

        // If there are no control bytes, scan over the simple8b block.
        uint8_t size = bsoncolumn::numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
        pos += size + 1;
    }

    return false;
};

// There are two decoding APIs. For all data that pass validation, both decoder implementations
// must produce the same results.
extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    using namespace mongo;

    // Skip inputs that do not pass validation.
    if (!validateBSONColumn(Data, Size).isOK()) {
        return 0;
    }

    // Set up both APIs.
    BSONColumn column(Data, Size);
    bsoncolumn::BSONColumnBlockBased block(Data, Size);
    boost::intrusive_ptr allocator{new BSONElementStorage()};
    std::vector<BSONElement> iteratorElems = {};
    std::vector<BSONElement> blockBasedElems = {};
    std::string blockBasedError;
    std::string iteratorError;

    // Attempt to decompress using the block-based API.
    try {
        block.decompress<bsoncolumn::BSONElementMaterializer, std::vector<BSONElement>>(
            blockBasedElems, allocator);
    } catch (const DBException& e) {
        blockBasedError = e.toString();
    }

    // Attempt to decompress using the iterator API.
    try {
        for (auto&& elem : column) {
            iteratorElems.push_back(elem);
        };
    } catch (const DBException& e) {
        iteratorError = e.toString();
    }

    // If one API failed, then both APIs must fail.
    if (!iteratorError.empty() || !blockBasedError.empty()) {
        invariant(!(iteratorError.empty() || blockBasedError.empty()),
                  str::stream() << "For the input: " << base64::encode(StringData(Data, Size))
                                << ". Iterator API returned "
                                << (iteratorError.empty() ? "results" : iteratorError)
                                << ". The block based API returned "
                                << (blockBasedError.empty() ? "results" : blockBasedError));
        return 0;
    }

    // If both APIs succeeded, the results must be the same.
    invariant(iteratorElems.size() == blockBasedElems.size(),
              str::stream() << "For the input: " << base64::encode(StringData(Data, Size))
                            << " the number of elements decompressed is different.");

    auto it = iteratorElems.begin();
    for (auto&& elem : blockBasedElems) {
        invariant(elem.binaryEqualValues(*it),
                  str::stream() << "For the input: " << base64::encode(StringData(Data, Size))
                                << ". The block-based API returned: " << elem.toString()
                                << ". The iterator API returned: " << (*it).toString());
        ++it;
    }
    return 0;
}
