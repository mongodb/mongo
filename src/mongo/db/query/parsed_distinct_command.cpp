// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/parsed_distinct_command.h"

#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_request_helper.h"

namespace mongo {

namespace {

/**
 * Checks dotted field for a projection and truncates the field name if we could be projecting on an
 * array element. Sets 'isIDOut' to true if the projection is on a sub document of _id. For example,
 * _id.a.2, _id.b.c.
 */
std::string getProjectedDottedField(const std::string& field, bool* isIDOut) {
    // Check if field contains an array index.
    std::vector<std::string> res;
    str::splitStringDelim(field, &res, '.');

    // Since we could exit early from the loop,
    // we should check _id here and set '*isIDOut' accordingly.
    *isIDOut = ("_id" == res[0]);

    // Skip the first dotted component. If the field starts
    // with a number, the number cannot be an array index.
    int arrayIndex = 0;
    for (size_t i = 1; i < res.size(); ++i) {
        if (mongo::NumberParser().base(10)(res[i], &arrayIndex).isOK()) {
            // Array indices cannot be negative numbers (this is not $slice).
            // Negative numbers are allowed as field names.
            if (arrayIndex >= 0) {
                // Generate prefix of field up to (but not including) array index.
                std::vector<std::string> prefixStrings(res);
                prefixStrings.resize(i);
                // Reset projectedField. Instead of overwriting, joinStringDelim() appends joined
                // string
                // to the end of projectedField.
                std::string projectedField;
                str::joinStringDelim(prefixStrings, &projectedField, '.');
                return projectedField;
            }
        }
    }

    return field;
}

/**
 * Add the stages that pull all values along a path and puts them into an array. Includes the
 * necessary unwind stage that can turn those into individual documents.
 */
std::vector<BSONObj> addReplaceRootForDistinct(const FieldPath& path) {
    std::vector<BSONObj> pipeline;
    BSONObjBuilder reshapeStageBuilder;
    reshapeStageBuilder.append(
        DocumentSourceReplaceRoot::kStageName,
        BSON("newRoot" << BSON(CanonicalDistinct::kUnwoundArrayFieldForViewUnwind
                               << BSON("$_internalFindAllValuesAtPath" << path.fullPath()))));
    pipeline.push_back(reshapeStageBuilder.obj());
    BSONObjBuilder unwindStageBuilder;
    {
        BSONObjBuilder unwindBuilder(unwindStageBuilder.subobjStart("$unwind"));
        unwindBuilder.append(
            "path", str::stream() << "$" << CanonicalDistinct::kUnwoundArrayFieldForViewUnwind);
        unwindBuilder.append("preserveNullAndEmptyArrays", true);
    }
    pipeline.push_back(unwindStageBuilder.obj());
    return pipeline;
}

std::unique_ptr<CollatorInterface> resolveCollator(OperationContext* opCtx,
                                                   const DistinctCommandRequest& distinct) {
    const auto& collation = distinct.getCollation();

    if (!collation || collation->isEmpty()) {
        return nullptr;
    }
    return uassertStatusOKWithContext(
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(*collation),
        "unable to parse collation");
}

}  // namespace

namespace parsed_distinct_command {

BSONObj getDistinctProjection(const std::string& field) {
    std::string projectedField(field);

    bool isID = false;
    if ("_id" == field) {
        isID = true;
    } else if (str::contains(field, '.')) {
        projectedField = getProjectedDottedField(field, &isID);
    }
    BSONObjBuilder bob;
    if (!isID) {
        bob.append("_id", 0);
    }
    bob.append(projectedField, 1);
    return bob.obj();
}

std::unique_ptr<ParsedDistinctCommand> parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<DistinctCommandRequest> distinctCommand,
    const ExtensionsCallback& extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures) {

    auto parsedDistinct = std::make_unique<ParsedDistinctCommand>();

    // Query.
    static BSONObj emptyQuery;
    auto query = distinctCommand->getQuery().get_value_or(emptyQuery);

    parsedDistinct->query = uassertStatusOK(
        MatchExpressionParser::parse(query, expCtx, extensionsCallback, allowedFeatures));

    // Collator.
    parsedDistinct->collator = resolveCollator(expCtx->getOperationContext(), *distinctCommand);
    if (parsedDistinct->collator.get() && expCtx->getCollator()) {
        tassert(11320919,
                "'parsedDistinct' and 'expCtx' collators do not match",
                CollatorInterface::collatorsMatch(parsedDistinct->collator.get(),
                                                  expCtx->getCollator()));
    }

    // Rest of the command.
    parsedDistinct->distinctCommandRequest = std::move(distinctCommand);

    return parsedDistinct;
}

AggregateCommandRequest asAggregation(const CanonicalQuery& query,
                                      boost::optional<ExplainOptions::Verbosity> verbosity,
                                      const SerializationContext& serializationContext) {
    tassert(9245502, "Expected distinct property on CanonicalQuery", query.getDistinct());

    const FindCommandRequest& findCommand = query.getFindCommandRequest();
    tassert(ErrorCodes::BadValue,
            "Unsupported type UUID for namespace",
            findCommand.getNamespaceOrUUID().isNamespaceString());

    // Build a pipeline that accomplishes the distinct request. The building code constructs a
    // pipeline that looks like this, assuming the distinct is on the key "a.b.c"
    //
    //      [
    //          { $match: { ... } },
    //          { $replaceRoot: {newRoot: {$_internalFindAllValuesAtPath: "a.b.c"}}},
    //          { $unwind: { path: "_internalUnwoundField", preserveNullAndEmptyArrays: true } },
    //          { $group: { _id: null, distinct: { $addToSet: "$<key>" } } }
    //      ]
    //
    // The purpose of the intermediate $replaceRoot and $unwind stages is to deal with cases
    // where there is an array along the distinct path. For example, if we're distincting on "a.b"
    // and have a document like {a: [{b: 1}, {b: 2}]}, distinct() should produce two values: 1
    // and 2.
    std::vector<BSONObj> pipeline;

    if (!findCommand.getFilter().isEmpty()) {
        BSONObjBuilder matchStageBuilder;
        matchStageBuilder.append("$match", findCommand.getFilter());
        pipeline.push_back(matchStageBuilder.obj());
    }

    FieldPath path(query.getDistinct()->getKey());
    const auto next_pipeline = addReplaceRootForDistinct(path);
    pipeline.insert(pipeline.end(), next_pipeline.begin(), next_pipeline.end());

    BSONObjBuilder groupStageBuilder;
    {
        BSONObjBuilder groupBuilder(groupStageBuilder.subobjStart("$group"));
        groupBuilder.appendNull("_id");
        {
            BSONObjBuilder distinctBuilder(groupBuilder.subobjStart("distinct"));
            distinctBuilder.append(
                "$addToSet",
                str::stream() << "$" << CanonicalDistinct::kUnwoundArrayFieldForViewUnwind);
            distinctBuilder.doneFast();
        }
        groupBuilder.doneFast();
    }
    pipeline.push_back(groupStageBuilder.obj());

    AggregateCommandRequest aggregateRequest(
        query.nss(), std::move(pipeline), serializationContext);

    aggregateRequest.setCollation(findCommand.getCollation());
    aggregateRequest.setHint(findCommand.getHint());


    int maxTimeMS = findCommand.getMaxTimeMS() ? static_cast<int>(*findCommand.getMaxTimeMS()) : 0;
    if (maxTimeMS > 0) {
        aggregateRequest.setMaxTimeMS(maxTimeMS);
    }

    if (const auto& rc = findCommand.getReadConcern(); rc && !rc->isEmpty()) {
        aggregateRequest.setReadConcern(rc);
    }

    if (findCommand.getUnwrappedReadPref() && !findCommand.getUnwrappedReadPref()->isEmpty()) {
        aggregateRequest.setUnwrappedReadPref(findCommand.getUnwrappedReadPref());
    }

    // Specify the 'cursor' option so that aggregation uses the cursor interface.
    aggregateRequest.setCursor(mongo::SimpleCursorOptions(serializationContext));

    aggregateRequest.setDbName(query.nss().dbName());

    if (verbosity) {
        aggregateRequest.setExplain(true);
    }

    return aggregateRequest;
}

std::unique_ptr<CanonicalQuery> parseCanonicalQuery(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<ParsedDistinctCommand> parsedDistinct,
    const CollatorInterface* defaultCollator) {
    auto& distinctRequest = *parsedDistinct->distinctCommandRequest;
    uassert(31032,
            "Key field cannot contain an embedded null byte",
            distinctRequest.getKey().find('\0') == std::string::npos);

    auto findRequest = std::make_unique<FindCommandRequest>(expCtx->getNamespaceString());
    if (auto query = distinctRequest.getQuery()) {
        findRequest->setFilter(query->getOwned());
    }
    auto projection = getDistinctProjection(std::string(distinctRequest.getKey()));
    const bool isDistinctMultiplanningEnabled =
        expCtx->isFeatureFlagShardFilteringDistinctScanEnabled();
    if (!isDistinctMultiplanningEnabled) {
        findRequest->setProjection(projection);
    }
    findRequest->setHint(distinctRequest.getHint().getOwned());
    if (auto& rc = distinctRequest.getReadConcern()) {
        findRequest->setReadConcern(rc);
    }
    if (auto maxTimeMS = distinctRequest.getMaxTimeMS()) {
        findRequest->setMaxTimeMS(maxTimeMS);
    }
    if (auto collation = distinctRequest.getCollation()) {
        findRequest->setCollation(collation.get().getOwned());
    }
    if (auto& queryOptions = distinctRequest.getUnwrappedReadPref()) {
        findRequest->setUnwrappedReadPref(queryOptions->getOwned());
    }

    auto parsedFind = uassertStatusOK(
        ParsedFindCommand::withExistingFilter(expCtx,
                                              std::move(parsedDistinct->collator),
                                              std::move(parsedDistinct->query),
                                              std::move(findRequest),
                                              ProjectionPolicies::findProjectionPolicies()));

    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = expCtx, .parsedFind = std::move(parsedFind)});

    cq->setDistinct(CanonicalDistinct(
        std::string{distinctRequest.getKey()},
        distinctRequest.getMirrored().value_or(false),
        distinctRequest.getSampleId(),
        isDistinctMultiplanningEnabled ? boost::make_optional(projection) : boost::none));

    return cq;
}

}  // namespace parsed_distinct_command
}  // namespace mongo
