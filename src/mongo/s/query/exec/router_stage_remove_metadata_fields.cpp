/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/query/exec/router_stage_remove_metadata_fields.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <absl/container/flat_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

RouterStageRemoveMetadataFields::RouterStageRemoveMetadataFields(
    OperationContext* opCtx, std::unique_ptr<RouterExecStage> child, StringDataSet metadataFields)
    : RouterExecStage(opCtx, std::move(child)), _metaFields(std::move(metadataFields)) {
    for (auto&& fieldName : _metaFields) {
        invariant(fieldName[0] == '$');  // We use this information to optimize next().
    }
}

StatusWith<ClusterQueryResult> RouterStageRemoveMetadataFields::next() {
    auto childResult = getChildStage()->next();
    if (!childResult.isOK() || childResult.getValue().isEOF()) {
        return childResult;
    }

    BSONObjIterator iterator(*childResult.getValue().getResult());

    // Find the first field that we need to remove.
    for (; iterator.more(); ++iterator) {
        // To save some time, we ensure that the current field name starts with a $
        // before checking if it's actually a metadata field in the map.
        if ((*iterator).fieldName()[0] == '$' && _metaFields.contains((*iterator).fieldName())) {
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
        if (!_metaFields.contains((*iterator).fieldNameStringData())) {
            builder.append(*iterator);
        }
    }
    return ClusterQueryResult(builder.obj(), childResult.getValue().getShardId());
}

}  // namespace mongo
