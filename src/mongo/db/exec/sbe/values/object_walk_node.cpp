/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/object_walk_node.h"

namespace mongo::sbe::value {
void FilterPositionInfoRecorder::recordValue(TypeTags tag, Value val) {
    auto [cpyTag, cpyVal] = copyValue(tag, val);
    outputArr->push_back(cpyTag, cpyVal);
    posInfo.back()++;
    isNewDoc = false;
}

void FilterPositionInfoRecorder::emptyArraySeen() {
    arraySeen = true;
}

void FilterPositionInfoRecorder::newDoc() {
    posInfo.push_back(0);
    isNewDoc = true;
    arraySeen = false;
}

void FilterPositionInfoRecorder::endDoc() {
    if (isNewDoc) {
        // We recorded no values for this doc. There are two possibilities:
        // (1) there is/are only empty array(s) at this path, which were traversed.
        //     Example: document {a: {b: []}}, path Get(a)/Traverse/Get(b)/Traverse/Id.
        // (2) There are no values at this path.
        //     Example: document {a: {b:1}} searching for path Get(a)/Traverse/Get(c)/Id.
        //
        // It is important that we distinguish these two cases for MQL's sake.
        if (arraySeen) {
            // This represents case (1). We record this by adding no values to our output and
            // leaving the position info value as '0'.

            // Do nothing.
        } else {
            // This represents case (2). We record this by adding an explicit Nothing value and
            // indicate that there's one value for this document. This matches the behavior of the
            // scalar traverseF primitive.
            outputArr->push_back(value::TypeTags::Nothing, Value(0));
            posInfo.back()++;
        }
    }
}

std::unique_ptr<HeterogeneousBlock> FilterPositionInfoRecorder::extractValues() {
    auto out = std::move(outputArr);
    outputArr = std::make_unique<HeterogeneousBlock>();
    return out;
}

void BlockProjectionPositionInfoRecorder::newDoc() {
    isNewDoc = true;
}

void BlockProjectionPositionInfoRecorder::endDoc() {
    if (isNewDoc) {
        // We didn't record anything for the last document, so add a Nothing to our block.
        outputArr->push_back(TypeTags::Nothing, Value(0));
    }
    isNewDoc = false;
}

std::unique_ptr<HeterogeneousBlock> BlockProjectionPositionInfoRecorder::extractValues() {
    auto out = std::move(outputArr);
    outputArr = std::make_unique<HeterogeneousBlock>();
    return out;
}

TagValueMaybeOwned ScalarProjectionPositionInfoRecorder::extractValue() {
    tassert(10926202, "expects empty arrayStack", arrayStack.empty());
    return std::move(outputValue);
}

}  // namespace mongo::sbe::value
