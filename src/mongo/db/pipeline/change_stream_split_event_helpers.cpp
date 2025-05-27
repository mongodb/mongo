/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream_split_event_helpers.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <list>
#include <map>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace change_stream_split_event {

std::pair<Document, size_t> processChangeEventBeforeSplit(const Document& event,
                                                          bool withMetadata) {
    if (withMetadata) {
        auto eventBson = event.toBsonWithMetaData<BSONObj::LargeSizeTrait>();
        return {Document::fromBsonWithMetaData(eventBson), eventBson.objsize()};
    } else {
        // Serialize just the user data, and add the metadata fields separately.
        auto eventBson = event.toBson<BSONObj::LargeSizeTrait>();
        MutableDocument mutDoc(Document{eventBson});
        mutDoc.copyMetaDataFrom(event);
        return {mutDoc.freeze(), eventBson.objsize()};
    }
}

std::queue<Document> splitChangeEvent(const Document& event,
                                      size_t maxFragmentBsonSize,
                                      size_t skipFirstFragments) {
    // Extract the underlying BSON. We expect the event to be trivially convertible either with
    // or without metadata, so we attempt to optimize the serialization here.
    auto eventBson =
        (event.isTriviallyConvertible() ? event.toBson<BSONObj::LargeSizeTrait>()
                                        : event.toBsonWithMetaData<BSONObj::LargeSizeTrait>());

    // Construct a sorted map of fields ordered by size and key for a deterministic greedy strategy
    // to minimize the total number of fragments (the first fragment contains as many fields as
    // possible). Don't include the original '_id' field, since each fragment will have its own.
    std::map<std::pair<size_t, std::string>, Value> sortedFieldMap;
    for (auto it = event.fieldIterator(); it.more();) {
        auto&& [key, value] = it.next();
        if (key != kIdField) {
            sortedFieldMap.emplace(std::make_pair(eventBson[key].size(), key), value);
        }
    }

    uassert(7182502,
            "Cannot split an empty event or an event containing solely '_id' field",
            !sortedFieldMap.empty());

    auto resumeTokenData =
        ResumeToken::parse(event.metadata().getSortKey().getDocument()).getData();

    std::list<MutableDocument> fragments;
    for (auto it = sortedFieldMap.cbegin(); it != sortedFieldMap.cend();) {
        // Update the resume token with the index of the fragment we're about to add.
        resumeTokenData.fragmentNum = fragments.size();

        // Add a new fragment at the end of the fragments list.
        auto& fragment = fragments.emplace_back();

        // Add fields required by all fragments.
        ResumeToken token(resumeTokenData);
        fragment.metadata().setSortKey(Value(token.toDocument()), true);
        fragment.addField(kIdField, fragment.metadata().getSortKey());
        fragment.addField(kSplitEventField,
                          Value(Document{{kFragmentNumberField, static_cast<int>(fragments.size())},
                                         {kTotalFragmentsField, 0}}));

        auto fragmentBsonSize = static_cast<size_t>(fragment.peek().toBsonWithMetaData().objsize());

        // Fill the fragment with as many fields as we can until we run out or exceed max size.
        // Always make sure we add at least one new field on each iteration.
        do {
            fragment.addField(it->first.second /* field name */, it->second /* field value */);
            fragmentBsonSize += it->first.first /* field size */;
        } while (++it != sortedFieldMap.cend() &&
                 fragmentBsonSize + it->first.first /* field size */ <= maxFragmentBsonSize);

        uassert(7182500,
                str::stream() << "Splitting change event failed: fragment size " << fragmentBsonSize
                              << " is greater than maximum allowed fragment size "
                              << maxFragmentBsonSize,
                fragmentBsonSize <= maxFragmentBsonSize);
    }

    // Iterate over the fragments to populate the 'kTotalFragmentsField' field and freeze the final
    // events.
    const auto totalFragments = Value(static_cast<int>(fragments.size()));
    const auto totalFragmentsFieldPath =
        FieldPath::getFullyQualifiedPath(kSplitEventField, kTotalFragmentsField);

    std::queue<Document> outputFragments;
    for (auto [it, i] = std::make_pair(fragments.begin(), 0ULL); it != fragments.end(); ++it, ++i) {
        // Do not insert first 'skipFirstFragments' into the output.
        if (i >= skipFirstFragments) {
            it->setNestedField(totalFragmentsFieldPath, totalFragments);
            outputFragments.push(it->freeze());
        }
    }

    return outputFragments;
}

}  // namespace change_stream_split_event
}  // namespace mongo
