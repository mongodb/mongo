// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/router_stage_remove_metadata_fields.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/document.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

template <typename RemovePolicy>
RouterStageRemoveFields<RemovePolicy>::RouterStageRemoveFields(
    OperationContext* opCtx, std::unique_ptr<RouterExecStage> child)
    : RouterExecStage(opCtx, std::move(child)) {}

template <typename RemovePolicy>
StatusWith<ClusterQueryResult> RouterStageRemoveFields<RemovePolicy>::next() {
    auto childResult = getChildStage()->next();
    if (!childResult.isOK() || childResult.getValue().isEOF()) {
        return childResult;
    }

    BSONObjIterator iterator(*childResult.getValue().getResult());

    // Find the first field that we need to remove.
    for (; iterator.more(); ++iterator) {
        if (RemovePolicy::shouldRemove((*iterator).fieldNameStringData())) {
            break;
        }
    }

    if (!iterator.more()) {
        // We got all the way to the end without finding any fields to remove, just return the whole
        // document.
        return childResult;
    }

    // Copy everything up to the first metadata field.
    const auto firstElementBufferStart =
        childResult.getValue().getResult()->firstElement().rawdata();
    auto endOfNonMetaFieldBuffer = (*iterator).rawdata();
    BSONObjBuilder builder;
    builder.bb().appendBuf(firstElementBufferStart,
                           endOfNonMetaFieldBuffer - firstElementBufferStart);

    // Copy any remaining fields that are not metadata. We expect metadata fields are likely to be
    // at the end of the document, so there is likely nothing else to copy.
    while ((++iterator).more()) {
        if (!RemovePolicy::shouldRemove((*iterator).fieldNameStringData())) {
            builder.append(*iterator);
        }
    }
    return ClusterQueryResult(builder.obj(), childResult.getValue().getShardId());
}

template class RouterStageRemoveFields<RemoveAllMetadataFieldsPolicy>;
template class RouterStageRemoveFields<RemoveSortKeyPolicy>;

}  // namespace mongo
