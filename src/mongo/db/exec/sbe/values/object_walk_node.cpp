// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
