// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/version_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalSplitPipeline,
                                     InternalSplitPipelineLiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalSplitPipeline,
                                                   DocumentSourceInternalSplitPipeline,
                                                   InternalSplitPipelineStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalSplitPipeline, DocumentSourceInternalSplitPipeline::id);

constexpr std::string_view DocumentSourceInternalSplitPipeline::kStageName;

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalSplitPipeline::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$_internalSplitPipeline must take a nested object but found: "
                          << elem,
            elem.type() == BSONType::object);

    auto specObj = elem.embeddedObject();

    HostTypeRequirement mergeType = HostTypeRequirement::kNone;
    boost::optional<ShardId> mergeShardId = boost::none;
    for (auto&& elt : specObj) {
        if (elt.fieldNameStringData() == "mergeType"sv) {
            const auto type = elt.type();

            if (type == BSONType::string) {
                auto mergeTypeString = elt.valueStringData();
                if ("localOnly"sv == mergeTypeString) {
                    mergeType = HostTypeRequirement::kReceivingHostOnly;
                } else if ("anyShard"sv == mergeTypeString) {
                    mergeType = HostTypeRequirement::kTargetedShards;
                } else if ("router"sv == mergeTypeString || "mongos"sv == mergeTypeString) {
                    mergeType = HostTypeRequirement::kRouter;
                } else {
                    uasserted(ErrorCodes::BadValue,
                              str::stream() << "unrecognized field while parsing mergeType: '"
                                            << mergeTypeString << "'");
                }
            } else if (type == BSONType::object) {
                auto specificShardObj = elt.Obj();
                auto specificShardElem = specificShardObj.getField("specificShard"sv);
                uassert(7958300,
                        "Object argument to $_internalSplitPipeline must contain a single string "
                        "field named 'specificShard'",
                        specificShardObj.nFields() == 1 &&
                            specificShardElem.type() == BSONType::string);

                auto* opCtx = expCtx->getOperationContext();
                auto* grid = Grid::get(opCtx);
                uassert(
                    7958301,
                    "Cannot specify 'mergeType' of 'specificShard' and not have sharding enabled",
                    grid && grid->isInitialized() && grid->isShardingInitialized());

                // Verify that the specified shardId references an actual shard.
                auto shardName = specificShardElem.str();
                ShardId shardId(shardName);
                uassertStatusOK(grid->shardRegistry()->getShard(opCtx, shardId));
                mergeShardId = shardId;
            } else {
                uasserted(ErrorCodes::BadValue,
                          str::stream()
                              << "'mergeType' must be a string value or an object but found: "
                              << type);
            }
        } else {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "unrecognized field while parsing $_internalSplitPipeline: '"
                                    << elt.fieldNameStringData() << "'");
        }
    }

    return new DocumentSourceInternalSplitPipeline(expCtx, mergeType, mergeShardId);
}

Value DocumentSourceInternalSplitPipeline::serialize(
    const query_shape::SerializationOptions& opts) const {
    std::string mergeTypeString;
    Document specificShardDoc;

    switch (_mergeType) {
        case HostTypeRequirement::kTargetedShards:
            mergeTypeString = "anyShard";
            break;

        case HostTypeRequirement::kReceivingHostOnly:
            mergeTypeString = "localOnly";
            break;

        case HostTypeRequirement::kRouter:
            if (feature_flags::gFeatureFlagAggMongosToRouter.isEnabled(
                    VersionContext::getDecoration(getExpCtx()->getOperationContext()),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                mergeTypeString = "router";
            } else {
                mergeTypeString = "mongos";
            }
            break;

        case HostTypeRequirement::kNone:
            if (_mergeShardId.has_value()) {
                specificShardDoc = Document{{"specificShard", Value(_mergeShardId->toString())}};
            }
            break;
        default:
            break;
    }

    return Value(Document{
        {getSourceName(),
         Value{Document{{"mergeType",
                         mergeTypeString.empty()
                             ? (specificShardDoc.empty() ? Value() : Value(specificShardDoc))
                             : Value(mergeTypeString)}}}}});
}

}  // namespace mongo
