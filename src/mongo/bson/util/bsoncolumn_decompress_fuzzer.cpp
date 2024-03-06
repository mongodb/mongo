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
#include "mongo/bson/util/bsoncolumn.h"

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
    BSONColumnBlockBased block(Data, Size);
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<BSONElement> iteratorElems = {};
    std::vector<BSONElement> blockBasedElems = {};
    std::string blockBasedError;
    std::string iteratorError;

    // Attempt to decompress using the block-based API.
    try {
        block.decompressIterative<BSONElementMaterializer, std::vector<BSONElement>>(
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

    // The logger does not work with the fuzzer, so we manually construct the error message.
    static auto printErrMsg = [&]() {
        auto printDecoderResult = [&](const std::vector<BSONElement>& elems) {
            std::string res = "{";
            for (auto&& elem : elems) {
                res += elem.toString() + ", ";
            }
            res += "}";
            return res;
        };

        return "Returned results are not equal. Iterator API returned " +
            (iteratorError.empty() ? printDecoderResult(iteratorElems)
                                   : "error: " + iteratorError) +
            ". The block based API returned " +
            (blockBasedError.empty() ? printDecoderResult(blockBasedElems)
                                     : "error: " + blockBasedError);
    };

    // If one API failed, then both APIs must fail.
    if (!iteratorError.empty() || !blockBasedError.empty()) {
        invariant(!(iteratorError.empty() || blockBasedError.empty()), printErrMsg());
        return 0;
    }

    // If both APIs succeeded, the results must be the same.
    invariant(iteratorElems.size() == blockBasedElems.size(), printErrMsg());

    auto it = iteratorElems.begin();
    for (auto&& elem : blockBasedElems) {
        invariant(elem.binaryEqualValues(*it), printErrMsg());
        ++it;
    }
    return 0;
}
