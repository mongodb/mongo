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

StatusWith<BSONObj> ResolvedView::asExpandedViewAggregation(
    const AggregationRequest& request) const {
    BSONObjBuilder aggregationBuilder;
    // Perform the aggregation on the resolved namespace.
    aggregationBuilder.append("aggregate", _namespace.coll());

    // The new pipeline consists of two parts: first, 'pipeline' in this ResolvedView;
    // then, the pipeline in 'request'.
    BSONArrayBuilder pipelineBuilder(aggregationBuilder.subarrayStart("pipeline"));
    for (auto&& item : _pipeline) {
        pipelineBuilder.append(item);
    }

    for (auto&& item : request.getPipeline()) {
        pipelineBuilder.append(item);
    }
    pipelineBuilder.doneFast();

    // The cursor option is always specified regardless of the presence of batchSize.
    if (request.getBatchSize()) {
        BSONObjBuilder batchSizeBuilder(aggregationBuilder.subobjStart("cursor"));
        batchSizeBuilder.append(AggregationRequest::kBatchSizeName, *request.getBatchSize());
        batchSizeBuilder.doneFast();
    } else {
        aggregationBuilder.append("cursor", BSONObj());
    }

    if (request.isExplain())
        aggregationBuilder.append("explain", true);

    return aggregationBuilder.obj();
}

StatusWith<BSONObj> ResolvedView::asExpandedViewAggregation(const BSONObj& aggCommand) const {
    auto aggRequest = AggregationRequest::parseFromBSON(_namespace, aggCommand);
    if (!aggRequest.isOK()) {
        return aggRequest.getStatus();
    }

    return asExpandedViewAggregation(aggRequest.getValue());
}

}  // namespace mongo
