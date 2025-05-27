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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/exec/document_value/document.h"

#include <cstddef>
#include <queue>
#include <utility>

namespace mongo {
namespace change_stream_split_event {

constexpr auto kIdField = "_id"_sd;
constexpr auto kSplitEventField = "splitEvent"_sd;
constexpr auto kFragmentNumberField = "fragment"_sd;
constexpr auto kTotalFragmentsField = "of"_sd;

/**
 * Calculates BSON size by serializing the event to BSON. Ensures that the serialization is
 * re-usable. The parameter 'withMetadata' desides whether the metadata is counted.
 * Also returns a new document optimized for later serialization by PlanExecutorPipeline.
 */
std::pair<Document, size_t> processChangeEventBeforeSplit(const Document& event, bool withMetadata);

/**
 * Splits the given change stream 'event' to several sub-events, called fragments. The size of BSON
 * serialization of each fragment does not exceed the given maximum fragment size. Each fragment
 * carries additionally fragment's ordinal number and the total number of fragments. Each fragment
 * has its own resume token as its '_id' and the sort key. In the resume scenario, the
 * 'skipFirstFragments' parameter indicates how many fragments were already received by the client
 * and can be skipped. For example, the following change event
 *     {_id: "RESUMETOKEN1", fullDocument: ..., fullDocumentBeforeChange: ..., ...}
 * can be split into the following fragments
 *     {_id: "RESUMETOKEN2", splitEvent{fragment: 1, of: 2}, fullDocumentBeforeChange: ...}
 *     {_id: "RESUMETOKEN3", splitEvent{fragment: 2, of: 2}, fullDocument: ...}
 */
std::queue<Document> splitChangeEvent(const Document& event,
                                      size_t maxFragmentBsonSize,
                                      size_t skipFirstFragments = 0);

}  // namespace change_stream_split_event
}  // namespace mongo
