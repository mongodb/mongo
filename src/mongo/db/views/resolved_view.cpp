/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/views/resolved_view.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {

bool ResolvedView::isResolvedViewErrorResponse(BSONObj commandResponseObj) {
    auto status = getStatusFromCommandResult(commandResponseObj);
    return ErrorCodes::CommandOnShardedViewNotSupportedOnMongod == status;
}

ResolvedView ResolvedView::fromBSON(BSONObj commandResponseObj) {
    uassert(40248,
            "command response expected to have a 'resolvedView' field",
            commandResponseObj.hasField("resolvedView"));

    auto viewDef = commandResponseObj.getObjectField("resolvedView");
    uassert(40249, "resolvedView must be an object", !viewDef.isEmpty());

    uassert(40250,
            "View definition must have 'ns' field of type string",
            viewDef.hasField("ns") && viewDef.getField("ns").type() == BSONType::String);

    uassert(40251,
            "View definition must have 'pipeline' field of type array",
            viewDef.hasField("pipeline") && viewDef.getField("pipeline").type() == BSONType::Array);

    std::vector<BSONObj> pipeline;
    for (auto&& item : viewDef["pipeline"].Obj()) {
        pipeline.push_back(item.Obj().getOwned());
    }

    return {ResolvedView(NamespaceString(viewDef["ns"].valueStringData()), pipeline)};
}

AggregationRequest ResolvedView::asExpandedViewAggregation(
    const AggregationRequest& request) const {
    // Perform the aggregation on the resolved namespace.  The new pipeline consists of two parts:
    // first, 'pipeline' in this ResolvedView; then, the pipeline in 'request'.
    std::vector<BSONObj> resolvedPipeline;
    resolvedPipeline.reserve(_pipeline.size() + request.getPipeline().size());
    resolvedPipeline.insert(resolvedPipeline.end(), _pipeline.begin(), _pipeline.end());
    resolvedPipeline.insert(
        resolvedPipeline.end(), request.getPipeline().begin(), request.getPipeline().end());

    AggregationRequest expandedRequest{_namespace, resolvedPipeline};

    if (request.getExplain()) {
        expandedRequest.setExplain(request.getExplain());
    } else {
        expandedRequest.setBatchSize(request.getBatchSize());
    }

    expandedRequest.setHint(request.getHint());
    expandedRequest.setComment(request.getComment());
    expandedRequest.setBypassDocumentValidation(request.shouldBypassDocumentValidation());
    expandedRequest.setAllowDiskUse(request.shouldAllowDiskUse());
    expandedRequest.setCollation(request.getCollation());

    return expandedRequest;
}

}  // namespace mongo
