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
#include "mongo/bson/column/bson_element_storage.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/column/bsoncolumn_expressions.h"
#include "mongo/bson/column/bsoncolumn_test_util.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/util/base64.h"

#include <string_view>

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
    std::string reopenError;

    // Attempt to decompress using the block-based API.
    try {
        block.decompress<bsoncolumn::BSONElementMaterializer>(blockBasedElems, allocator);
    } catch (const DBException& e) {
        blockBasedError = e.toString();
        invariant(e.code() == ErrorCodes::InvalidBSONColumn,
                  str::stream() << "For the input: " << base64::encode(std::string_view(Data, Size))
                                << ", the block based API failed with unexpected error code "
                                << e.code());
    }

    // Attempt to decompress using the iterator API.
    try {
        for (auto&& elem : column) {
            iteratorElems.push_back(elem);
        };
    } catch (const DBException& e) {
        iteratorError = e.toString();
        invariant(e.code() == ErrorCodes::InvalidBSONColumn,
                  str::stream() << "For the input: " << base64::encode(std::string_view(Data, Size))
                                << ", the iterator API failed with unexpected error code "
                                << e.code());
    }

    // Attempt to reopen using the reopen API.
    try {
        BSONColumnBuilder(Data, Size);
    } catch (const DBException& e) {
        reopenError = e.toString();
    }

    // If one API failed, then all APIs must fail.
    if (!iteratorError.empty() || !blockBasedError.empty() || !reopenError.empty()) {
        invariant(!(iteratorError.empty() || blockBasedError.empty() || reopenError.empty()),
                  str::stream() << "For the input: " << base64::encode(std::string_view(Data, Size))
                                << ". Iterator API returned "
                                << (iteratorError.empty() ? "results" : iteratorError)
                                << ". The block based API returned "
                                << (blockBasedError.empty() ? "results" : blockBasedError)
                                << ". The reopen API returned "
                                << (reopenError.empty() ? "results" : reopenError));
        return 0;
    }

    // If both APIs succeeded, the results must be the same.
    invariant(iteratorElems.size() == blockBasedElems.size(),
              str::stream() << "For the input: " << base64::encode(std::string_view(Data, Size))
                            << " the number of elements decompressed is different.");

    auto it = iteratorElems.begin();
    for (auto&& elem : blockBasedElems) {
        invariant(elem.binaryEqualValues(*it),
                  str::stream() << "For the input: " << base64::encode(std::string_view(Data, Size))
                                << ". The block-based API returned: " << elem.toString()
                                << ". The iterator API returned: " << (*it).toString());
        ++it;
    }

    if (iteratorError.empty()) {
        invariant(bsoncolumn::count(Data, Size) == iteratorElems.size());

        // Compute expected min/max from iterator elements.
        auto expected = bsoncolumn::expectedMinMax(iteratorElems);

        // min/max/minmax use a stricter decode path (decompressAllLiteral) that may throw on
        // inputs the iterator and block-based APIs accept. Catch here so the fuzzer doesn't
        // terminate via an unhandled exception; then assert all APIs agree.
        decltype(bsoncolumn::min<bsoncolumn::BSONElementMaterializer>(
            Data, Size, allocator)) minResult,
            maxResult;
        decltype(bsoncolumn::minmax<bsoncolumn::BSONElementMaterializer>(
            Data, Size, allocator)) minmaxResult;
        try {
            minResult = bsoncolumn::min<bsoncolumn::BSONElementMaterializer>(Data, Size, allocator);
            maxResult = bsoncolumn::max<bsoncolumn::BSONElementMaterializer>(Data, Size, allocator);
            minmaxResult =
                bsoncolumn::minmax<bsoncolumn::BSONElementMaterializer>(Data, Size, allocator);
        } catch (...) {
            // If min/max/minmax threw, the other decode APIs must also have succeeded; if they
            // didn't, this reveals an inconsistency between decode paths.
            invariant(false,
                      str::stream()
                          << "For the input: " << base64::encode(std::string_view(Data, Size))
                          << ". min/max/minmax threw: " << exceptionToStatus().toString()
                          << " but iterator and block-based APIs succeeded");
        }

        invariant(minResult.first.binaryEqualValues(expected.min.first),
                  str::stream() << "For the input: " << base64::encode(std::string_view(Data, Size))
                                << ". min() returned: " << minResult.first.toString()
                                << " but expected: " << expected.min.first.toString());
        if (!minResult.first.eoo()) {
            invariant(minResult.second == expected.min.second,
                      str::stream()
                          << "For the input: " << base64::encode(std::string_view(Data, Size))
                          << ". min() returned index " << minResult.second << " but expected index "
                          << expected.min.second);
        }

        invariant(maxResult.first.binaryEqualValues(expected.max.first),
                  str::stream() << "For the input: " << base64::encode(std::string_view(Data, Size))
                                << ". max() returned: " << maxResult.first.toString()
                                << " but expected: " << expected.max.first.toString());
        if (!maxResult.first.eoo()) {
            invariant(maxResult.second == expected.max.second,
                      str::stream()
                          << "For the input: " << base64::encode(std::string_view(Data, Size))
                          << ". max() returned index " << maxResult.second << " but expected index "
                          << expected.max.second);
        }

        auto [minmaxMin, minmaxMax] = minmaxResult;
        invariant(minmaxMin.binaryEqualValues(expected.min.first),
                  str::stream() << "For the input: " << base64::encode(std::string_view(Data, Size))
                                << ". minmax().first returned: " << minmaxMin.toString()
                                << " but expected: " << expected.min.first.toString());
        invariant(minmaxMax.binaryEqualValues(expected.max.first),
                  str::stream() << "For the input: " << base64::encode(std::string_view(Data, Size))
                                << ". minmax().second returned: " << minmaxMax.toString()
                                << " but expected: " << expected.max.first.toString());
    }

    // Verify dense: should be true iff no decompressed elements are EOO (missing).
    {
        bool hasMissing = std::any_of(iteratorElems.begin(),
                                      iteratorElems.end(),
                                      [](const BSONElement& e) { return e.eoo(); });
        bool result = bsoncolumn::dense(Data, Size);
        invariant(result != hasMissing,
                  str::stream() << "dense() returned " << result << " but hasMissing=" << hasMissing
                                << ". Column: " << base64::encode(std::string_view(Data, Size)));
    }

    return 0;
}
