/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/s/query/router_stage_remove_metadata_fields.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document.h"

namespace mongo {

RouterStageRemoveMetadataFields::RouterStageRemoveMetadataFields(
    OperationContext* opCtx,
    std::unique_ptr<RouterExecStage> child,
    std::vector<StringData> metadataFields)
    : RouterExecStage(opCtx, std::move(child)), _metaFields(std::move(metadataFields)) {
    for (auto&& fieldName : _metaFields) {
        invariant(fieldName[0] == '$');  // We use this information to optimize next().
    }
}

StatusWith<ClusterQueryResult> RouterStageRemoveMetadataFields::next(
    RouterExecStage::ExecContext execContext) {
    auto childResult = getChildStage()->next(execContext);
    if (!childResult.isOK() || !childResult.getValue().getResult()) {
        return childResult;
    }

    BSONObjIterator iterator(*childResult.getValue().getResult());
    // Find the first field that we need to remove.
    while (iterator.more() && (*iterator).fieldName()[0] != '$' &&
           std::find(_metaFields.begin(), _metaFields.end(), (*iterator).fieldNameStringData()) ==
               _metaFields.end()) {
        ++iterator;
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
        if (std::find(_metaFields.begin(), _metaFields.end(), (*iterator).fieldNameStringData()) ==
            _metaFields.end()) {
            builder.append(*iterator);
        }
    }
    return {builder.obj()};
}

}  // namespace mongo
