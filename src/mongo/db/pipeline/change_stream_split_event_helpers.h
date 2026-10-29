// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <queue>
#include <string_view>
#include <utility>

namespace mongo {
namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] change_stream_split_event {

inline constexpr std::string_view kIdField{"_id"};
inline constexpr std::string_view kSplitEventField{"splitEvent"};
inline constexpr std::string_view kFragmentNumberField{"fragment"};
inline constexpr std::string_view kTotalFragmentsField{"of"};

/**
 * Calculates BSON size by serializing the event to BSON. Ensures that the serialization is
 * re-usable. The parameter 'withMetadata' decides whether the metadata is counted.
 * Also returns a new document optimized for later serialization by PlanExecutorPipeline.
 */
std::pair<Document, size_t> processChangeEventBeforeSplit(const Document& event, bool withMetadata);

/**
 * Splits the given change stream 'event' to several sub-events, called fragments. The size of
 * BSON serialization of each fragment does not exceed the given maximum fragment size. Each
 * fragment carries additionally fragment's ordinal number and the total number of fragments.
 * Each fragment has its own resume token as its '_id' and the sort key. In the resume scenario,
 * the 'skipFirstFragments' parameter indicates how many fragments were already received by the
 * client and can be skipped. For example, the following change event
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
