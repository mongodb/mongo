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
#include "mongo/bson/column/bsoncolumn_test_util.h"
#include "mongo/bson/util/bsonobj_traversal.h"
#include "mongo/db/exec/sbe/values/value.h"

static bool isDataOnlyInterleaved(const char* binary, size_t size) {
    using namespace mongo;
    const char* pos = binary;
    const char* end = binary + size;

    // Must start with interleaved data.
    if (!bsoncolumn::isInterleavedStartControlByte(*pos)) {
        return false;
    }

    while (pos != end) {
        uint8_t control = *pos;
        if (control == stdx::to_underlying(BSONType::eoo)) {
            // Reached the end of interleaved mode and this should be the end of the binary.
            return *(++pos) == stdx::to_underlying(BSONType::eoo);
        }

        if (bsoncolumn::isInterleavedStartControlByte(control)) {
            BSONObj refObj{pos + 1};
            pos += refObj.objsize() + 1;
            continue;
        }

        if (bsoncolumn::isUncompressedLiteralControlByte(control)) {
            BSONElement literal(pos, 1, BSONElement::TrustedInitTag{});
            pos += literal.size();
            continue;
        }

        // If there are no control bytes, scan over the simple8b block.
        uint8_t size = bsoncolumn::numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
        pos += size + 1;
    }

    return false;
}

static bool containsDuplicateFields(mongo::BSONObj obj) {
    using namespace mongo;
    StringDataSet fields;
    for (auto&& elem : obj) {
        StringData fieldName = elem.fieldNameStringData();
        if (fields.contains(fieldName)) {
            return true;
        }
        fields.insert(fieldName);
        if (elem.isABSONObj() && containsDuplicateFields(elem.embeddedObject())) {
            return true;
        }
    };

    return false;
}

static void findAllScalarPaths(std::vector<mongo::sbe::value::CellBlock::Path>& paths,
                               const mongo::BSONElement& elem,
                               mongo::sbe::value::CellBlock::Path path,
                               bool previousIsArray) {
    using namespace mongo;
    if (!elem.isABSONObj()) {
        // Array elements have a field that is their index in the array. We shouldn't
        // append that field in the 'Path'.
        if (!previousIsArray) {
            path.push_back(sbe::value::CellBlock::Get{std::string{elem.fieldNameStringData()}});
        }
        path.push_back(sbe::value::CellBlock::Id{});
        paths.push_back(path);
        return;
    }

    BSONObj obj = elem.embeddedObject();

    if (elem.type() == BSONType::array) {
        // We only want to decompress an array if there is a single element.
        // TODO SERVER-90044 remove this condition.
        if (obj.nFields() != 1) {
            return;
        }

        // All array elements are turned into an object. The field is the index and the value is the
        // array element. We don't want to generate 'Path's for the fields that are indexes, so we
        // manually track when the element is an array element or an object.
        // If the next element is an object
        if (!previousIsArray) {
            path.push_back(sbe::value::CellBlock::Get{std::string{elem.fieldNameStringData()}});
        }
        path.push_back(sbe::value::CellBlock::Traverse{});
        findAllScalarPaths(paths, obj.firstElement(), path, true);
        return;
    }

    // Start a new path for each element in the sub-object.
    for (auto&& newElem : obj) {
        auto nPath = path;
        if (!previousIsArray) {
            nPath.push_back(sbe::value::CellBlock::Get{std::string{elem.fieldNameStringData()}});
            nPath.push_back(sbe::value::CellBlock::Traverse{});
        }
        findAllScalarPaths(paths, newElem, nPath, false);
    }
}

// There are two decoding APIs. For all data that pass validation, both decoder implementations must
// produce the same results. This fuzzer builds 'SBEPath' and only tests interleaved data.
extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    using namespace mongo;
    using SBEColumnMaterializer = sbe::bsoncolumn::SBEColumnMaterializer;

    // Skip inputs that do not pass validation.
    if (!validateBSONColumn(Data, Size).isOK()) {
        return 0;
    }

    // Skip inputs that do not start with interleaved data, or require exiting interleaved mode.
    if (!isDataOnlyInterleaved(Data, Size)) {
        return 0;
    }

    // Iterate through the reference object, find all scalar fields and construct a 'SBEPath' for
    // each field.
    const char* control = Data;
    BSONObj refObj{control + 1};
    if (containsDuplicateFields(refObj)) {
        return 0;
    }
    std::vector<std::pair<sbe::bsoncolumn::SBEPath, std::vector<SBEColumnMaterializer::Element>&>>
        blockBasedResults;
    // Required to keep each 'Container' for each 'SBEPath' in scope.
    std::vector<std::vector<SBEColumnMaterializer::Element>> containers;
    // Required to change the results of the iterator API into SBE values.
    std::vector<sbe::value::CellBlock::PathRequest> pathReqs;
    // Holds all the scalar field paths to decompress.
    std::vector<sbe::value::CellBlock::Path> fieldPaths;

    // Find all the fields including fields nested inside objects that we can decompress.
    for (auto&& elem : refObj) {
        findAllScalarPaths(fieldPaths, elem, {}, false);
    }

    // Set up 'SBEPath' for the block-based API, and 'PathRequest' for the iterator API. We need to
    // reserve the number of 'Container's we need to ensure the address remain the same when passed
    // to 'blockBasedResults'.
    containers.reserve(fieldPaths.size());
    for (auto&& fieldPath : fieldPaths) {
        containers.emplace_back();
        auto path = sbe::bsoncolumn::SBEPath{sbe::value::CellBlock::PathRequest(
            sbe::value::CellBlock::PathRequestType::kFilter, fieldPath)};
        blockBasedResults.push_back({path, containers.back()});
        pathReqs.push_back(path._pathRequest);
    }

    // Now we are ready to decompress. Set up both APIs.
    BSONColumn column(Data, Size);
    bsoncolumn::BSONColumnBlockBased block(Data, Size);
    boost::intrusive_ptr allocator{new BSONElementStorage()};
    std::vector<BSONObj> iteratorObjs;
    std::string blockBasedError;
    std::string iteratorError;

    // Attempt to decompress using the iterator API.
    try {
        for (auto&& elem : column) {
            if (elem.isABSONObj()) {
                iteratorObjs.push_back(elem.embeddedObject().getOwned());
                continue;
            }
            // Must be an EOO element.
            invariant(elem.type() == BSONType::eoo,
                      str::stream() << "Iterator API returned data that was not an object nor EOO: "
                                    << elem.toString());
            BSONObjBuilder bob;
            iteratorObjs.push_back(bob.obj());
        };
    } catch (const DBException& e) {
        iteratorError = e.toString();
    }

    // Attempt to decompress using the block-based API.
    try {
        block.decompress<SBEColumnMaterializer>(allocator, std::span(blockBasedResults));
    } catch (const DBException& e) {
        blockBasedError = e.toString();
    }

    // If one API failed, then both APIs must fail.
    if (!iteratorError.empty() || !blockBasedError.empty()) {
        invariant(!(iteratorError.empty() || blockBasedError.empty()),
                  str::stream() << ". Returned results are not equal. Iterator API "
                                << (iteratorError.empty() ? "returned results."
                                                          : "errored: " + iteratorError)
                                << ". The block based API "
                                << (blockBasedError.empty() ? "returned results."
                                                            : "errored: " + blockBasedError));
        return 0;
    }

    // If both APIs succeeded, the results must be the same. The iterator API returns full BSON
    // objects, but the block-based API returns SBE values for a particular 'SBEPath'. Therefore, we
    // have to extract the SBE values for the relevant paths from the iterator API results.
    std::vector<std::unique_ptr<sbe::value::CellBlock>> iteratorBlocks =
        sbe::value::extractCellBlocksFromBsons(pathReqs, iteratorObjs);

    // Must decompress the same number of 'SBEPath's.
    invariant(
        blockBasedResults.size() == iteratorBlocks.size(),
        str::stream()
            << "The number of paths decompressed is different. The iterator API decompressed: "
            << iteratorBlocks.size() << " paths. The block-based API decompressed "
            << blockBasedResults.size() << " paths.");

    // Validate the decompressed elements from the different APIs are the same for each 'SBEPath'.
    auto blockBasedRes = blockBasedResults.begin();
    for (auto&& iteratorBlock : iteratorBlocks) {
        auto blockElems = (*blockBasedRes).second;
        auto iteratorElems = iteratorBlock->getValueBlock().extract();

        // Must decompress the same number of elements.
        invariant(iteratorElems.count() == blockElems.size(),
                  str::stream() << "The number of elements decompressed is different. The iterator "
                                   "API decompressed: "
                                << iteratorElems.count()
                                << " elements. The block-based API decompressed "
                                << blockElems.size() << " elements. The path was "
                                << (*blockBasedRes).first._pathRequest.toString());

        // Each decompressed element must be identical.
        for (size_t i = 0; i < iteratorElems.count(); ++i) {
            SBEColumnMaterializer::Element blockElem = blockElems[i];
            SBEColumnMaterializer::Element iteratorElem = {iteratorElems.tags()[i],
                                                           iteratorElems.vals()[i]};

            // Converting the iterator results to SBE will always produce the tags 'StringBig' or
            // 'StringSmall' for strings, but the block-based API could use 'bsonString'. This
            // difference is expected, and the values should still be the same.
            bool iteratorTagIsAString = (iteratorElem.first == sbe::value::TypeTags::StringBig ||
                                         iteratorElem.first == sbe::value::TypeTags::StringSmall);
            invariant(
                iteratorElem.first == blockElem.first ||
                    (blockElem.first == sbe::value::TypeTags::bsonString && iteratorTagIsAString),
                str::stream() << "For the input: " << base64::encode(StringData(Data, Size))
                              << " For the path: " << (*blockBasedRes).first._pathRequest.toString()
                              << ". The types differ. Iterator API returned " << iteratorElem.first
                              << ". The block based API returned " << blockElem.first);

            invariant(bsoncolumn::areSBEBinariesEqual(blockElem, iteratorElem),
                      str::stream()
                          << "For the input: " << base64::encode(StringData(Data, Size))
                          << " For the path: " << (*blockBasedRes).first._pathRequest.toString()
                          << ".  The values differ. Iterator API returned "
                          << sbe::value::print(iteratorElem) << ". The block based API returned "
                          << sbe::value::print(blockElem));
        }
        ++blockBasedRes;
    }

    return 0;
}
