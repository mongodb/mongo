/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream_filter_helpers.h"

#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/change_stream_helpers_legacy.h"
#include "mongo/db/pipeline/change_stream_rewrite_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
namespace change_stream_filter {

std::unique_ptr<MatchExpression> buildTsFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp startFromInclusive,
    const MatchExpression* userMatch) {
    return MatchExpressionParser::parseAndNormalize(BSON("ts" << GTE << startFromInclusive),
                                                    expCtx);
}

std::unique_ptr<MatchExpression> buildFromMigrateSystemOpFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const MatchExpression* userMatch) {
    auto cmdNsRegex = DocumentSourceChangeStream::getCmdNsRegexForChangeStream(expCtx);

    // The filter {fromMigrate:true} allows quickly skip nonrelevant oplog entries
    auto andMigrateEvents = std::make_unique<AndMatchExpression>();
    andMigrateEvents->add(
        MatchExpressionParser::parseAndNormalize(BSON("fromMigrate" << true), expCtx));
    andMigrateEvents->add(
        MatchExpressionParser::parseAndNormalize(BSON("ns" << BSONRegEx(cmdNsRegex)), expCtx));

    auto orMigrateEvents = std::make_unique<OrMatchExpression>();
    auto collRegex = DocumentSourceChangeStream::getCollRegexForChangeStream(expCtx);
    orMigrateEvents->add(
        MatchExpressionParser::parseAndNormalize(BSON("o.create" << BSONRegEx(collRegex)), expCtx));
    orMigrateEvents->add(MatchExpressionParser::parseAndNormalize(
        BSON("o.createIndexes" << BSONRegEx(collRegex)), expCtx));
    andMigrateEvents->add(std::move(orMigrateEvents));
    return andMigrateEvents;
}

std::unique_ptr<MatchExpression> buildNotFromMigrateFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const MatchExpression* userMatch) {
    // Exclude any events that are marked as 'fromMigrate' in the oplog.
    auto fromMigrateFilter =
        MatchExpressionParser::parseAndNormalize(BSON("fromMigrate" << NE << true), expCtx);

    // If 'showSystemEvents' is set, however, we do return some specific 'fromMigrate' events.
    if (expCtx->changeStreamSpec->getShowSystemEvents()) {
        auto orMigrateEvents = std::make_unique<OrMatchExpression>(std::move(fromMigrateFilter));
        orMigrateEvents->add(buildFromMigrateSystemOpFilter(expCtx, userMatch));
        fromMigrateFilter = std::move(orMigrateEvents);
    }
    return fromMigrateFilter;
}

std::unique_ptr<MatchExpression> buildOperationFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const MatchExpression* userMatch) {

    // Regexes to match each of the necessary namespace components for the current stream type.
    auto nsRegex = DocumentSourceChangeStream::getNsRegexForChangeStream(expCtx);
    auto collRegex = DocumentSourceChangeStream::getCollRegexForChangeStream(expCtx);
    auto cmdNsRegex = DocumentSourceChangeStream::getCmdNsRegexForChangeStream(expCtx);

    auto streamType = DocumentSourceChangeStream::getChangeStreamType(expCtx->ns);

    /**
     * IMPORTANT: Any new operationType added here must also add corresponding oplog rewrites in the
     * file change_stream_rewrite_helpers.cpp. A number of the existing rewrite functions in that
     * file rely upon an exhaustive list of all change stream events that are derived directly from
     * the oplog. Without appropriate rewrite rules for the new event, the optimizer will assume
     * that no oplog entry can ever match the user's filter, causing it to discard those events.
     */

    // The standard event filter, before it is combined with the user filter, is as follows:
    //    {
    //      $or: [
    //        {ns: nsRegex, $nor: [{op: "n"}, {op: "c"}]},    // CRUD events
    //        {ns: cmdNsRegex, op: "c", $or: [                // Commands on relevant DB(s)
    //          {"o.drop": collRegex},                        // Drops of relevant collection(s)
    //          {"o.renameCollection": nsRegex},              // Renames of relevant collection(s)
    //          {"o.renameCollection": {$exists: true},       // Relevant collection was overwritten
    //              "o.to": nsRegex},
    //          {"o.dropDatabase": {$exists: true}}           // Omitted for single-coll streams
    //        ]}
    //      ]
    //    }

    // Top-level filter which will match against any of the categories of relevant events.
    std::unique_ptr<ListOfMatchExpression> operationFilter = std::make_unique<OrMatchExpression>();

    // (1) CRUD events on a monitored namespace.
    auto crudEvents = BSON("ns" << BSONRegEx(nsRegex) << "$nor"
                                << BSON_ARRAY(BSON("op"
                                                   << "n")
                                              << BSON("op"
                                                      << "c")));

    // (2.1) The namespace for matching relevant commands.
    auto cmdNsMatch = BSON("op"
                           << "c"
                           << "ns" << BSONRegEx(cmdNsRegex));

    // (2.2) Commands that are run on a monitored database and/or collection.
    auto dropEvent = BSON("o.drop" << BSONRegEx(collRegex));
    auto dropDbEvent = BSON("o.dropDatabase" << BSON("$exists" << true));
    auto renameFromEvent = BSON("o.renameCollection" << BSONRegEx(nsRegex));
    auto renameToEvent =
        BSON("o.renameCollection" << BSON("$exists" << true) << "o.to" << BSONRegEx(nsRegex));
    const auto createEvent = BSON("o.create" << BSONRegEx(collRegex));
    const auto createIndexesEvent = BSON("o.createIndexes" << BSONRegEx(collRegex));
    const auto commitIndexBuildEvent = BSON("o.commitIndexBuild" << BSONRegEx(collRegex));
    const auto dropIndexesEvent = BSON("o.dropIndexes" << BSONRegEx(collRegex));
    const auto collModEvent = BSON("o.collMod" << BSONRegEx(collRegex));

    auto orCmdEvents = std::make_unique<OrMatchExpression>();
    orCmdEvents->add(MatchExpressionParser::parseAndNormalize(dropEvent, expCtx));
    orCmdEvents->add(MatchExpressionParser::parseAndNormalize(renameFromEvent, expCtx));
    orCmdEvents->add(MatchExpressionParser::parseAndNormalize(renameToEvent, expCtx));
    orCmdEvents->add(MatchExpressionParser::parseAndNormalize(createEvent, expCtx));
    orCmdEvents->add(MatchExpressionParser::parseAndNormalize(createIndexesEvent, expCtx));
    orCmdEvents->add(MatchExpressionParser::parseAndNormalize(commitIndexBuildEvent, expCtx));
    orCmdEvents->add(MatchExpressionParser::parseAndNormalize(dropIndexesEvent, expCtx));
    orCmdEvents->add(MatchExpressionParser::parseAndNormalize(collModEvent, expCtx));

    // Omit dropDatabase on single-collection streams. While the stream will be invalidated before
    // it sees this event, the user will incorrectly see it if they startAfter the invalidate.
    if (streamType != DocumentSourceChangeStream::ChangeStreamType::kSingleCollection) {
        orCmdEvents->add(MatchExpressionParser::parseAndNormalize(dropDbEvent, expCtx));
    }

    // (2.3) Commands must match the cmd namespace AND one of the event filters.
    auto cmdEventsOnTargetDb = std::make_unique<AndMatchExpression>();
    cmdEventsOnTargetDb->add(MatchExpressionParser::parseAndNormalize(cmdNsMatch, expCtx));
    cmdEventsOnTargetDb->add(std::move(orCmdEvents));

    // (3) Put together the final standard filter.
    operationFilter->add(MatchExpressionParser::parseAndNormalize(crudEvents, expCtx));
    operationFilter->add(std::move(cmdEventsOnTargetDb));

    // (4) Apply the user's match filters to the standard event filter.
    if (auto rewrittenMatch = change_stream_rewrite::rewriteFilterForFields(expCtx, userMatch)) {
        operationFilter = std::make_unique<AndMatchExpression>(std::move(operationFilter), nullptr);
        operationFilter->add(std::move(rewrittenMatch));
    }

    return operationFilter;
}

std::unique_ptr<MatchExpression> buildViewDefinitionEventFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const MatchExpression* userMatch) {
    // The view op filter is as follows:
    // {
    //   ns: nsSystemViewsRegex, // match system.views for relevant DBs
    //   $nor: [                 // match only CRUD events
    //     {op: "n"},
    //     {op: "c"}
    //   ]
    // }
    auto nsSystemViewsRegex = DocumentSourceChangeStream::getViewNsRegexForChangeStream(expCtx);
    auto viewEventsFilter = BSON("ns" << BSONRegEx(nsSystemViewsRegex) << "$nor"
                                      << BSON_ARRAY(BSON("op"
                                                         << "n")
                                                    << BSON("op"
                                                            << "c")));

    return MatchExpressionParser::parseAndNormalize(viewEventsFilter, expCtx);
}

std::unique_ptr<MatchExpression> buildInvalidationFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const MatchExpression* userMatch) {
    auto nss = expCtx->ns;
    auto streamType = DocumentSourceChangeStream::getChangeStreamType(nss);

    // A whole-cluster change stream is not invalidated by anything.
    if (streamType == DocumentSourceChangeStream::ChangeStreamType::kAllChangesForCluster) {
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    BSONArrayBuilder invalidatingCommands;
    if (streamType == DocumentSourceChangeStream::ChangeStreamType::kSingleCollection) {
        // A single-collection stream is invalidated by drop and rename events.
        invalidatingCommands.append(BSON("o.drop" << nss.coll()));
        invalidatingCommands.append(BSON("o.renameCollection" << nss.ns()));
        invalidatingCommands.append(
            BSON("o.renameCollection" << BSON("$exists" << true) << "o.to" << nss.ns()));
    } else {
        // For a whole-db streams, only 'dropDatabase' will cause an invalidation event.
        invalidatingCommands.append(BSON("o.dropDatabase" << BSON("$exists" << true)));
    }

    // Match only against the target db's command namespace.
    auto invalidatingFilter = BSON("op"
                                   << "c"
                                   << "ns" << nss.getCommandNS().ns() << "$or"
                                   << invalidatingCommands.arr());
    return MatchExpressionParser::parseAndNormalize(invalidatingFilter, expCtx);
}  // namespace change_stream_filter

std::unique_ptr<MatchExpression> buildTransactionFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const MatchExpression* userMatch) {
    BSONObjBuilder applyOpsBuilder;

    // "o.applyOps" stores the list of operations, so it must be an array.
    applyOpsBuilder.append("op", "c");
    applyOpsBuilder.append("o.applyOps",
                           BSON("$type"
                                << "array"));
    applyOpsBuilder.append("o.prepare", BSON("$not" << BSON("$eq" << true)));
    applyOpsBuilder.append("o.partialTxn", BSON("$not" << BSON("$eq" << true)));
    {
        // Include this 'applyOps' if it has an operation with a matching namespace _or_ if it has a
        // 'prevOpTime' link to another 'applyOps' command, indicating a multi-entry transaction.
        BSONArrayBuilder orBuilder(applyOpsBuilder.subarrayStart("$or"));
        {
            // Regexes for full-namespace, collection, and command-namespace matching.
            auto nsRegex = DocumentSourceChangeStream::getNsRegexForChangeStream(expCtx);
            auto collRegex = DocumentSourceChangeStream::getCollRegexForChangeStream(expCtx);
            auto cmdNsRegex = DocumentSourceChangeStream::getCmdNsRegexForChangeStream(expCtx);

            // Match relevant CRUD events on the monitored namespaces.
            orBuilder.append(BSON("o.applyOps.ns" << BSONRegEx(nsRegex)));

            // Match relevant command events on the monitored namespaces.
            orBuilder.append(BSON(
                "o.applyOps" << BSON(
                    "$elemMatch" << BSON(
                        "ns" << BSONRegEx(cmdNsRegex)
                             << OR(BSON("o.create" << BSONRegEx(collRegex)),
                                   // We don't need to consider 'o.commitIndexBuild' here because
                                   // creating an index on a non-empty collection is not allowed.
                                   BSON("o.createIndexes" << BSONRegEx(collRegex)))))));

            // The default repl::OpTime is the value used to indicate a null "prevOpTime" link.
            orBuilder.append(BSON(repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName
                                  << BSON("$ne" << repl::OpTime().toBSON())));
        }
    }
    auto applyOpsFilter = applyOpsBuilder.obj();

    auto transactionFilter =
        MatchExpressionParser::parseAndNormalize(BSON(OR(applyOpsFilter,
                                                         BSON("op"
                                                              << "c"
                                                              << "o.commitTransaction" << 1))),
                                                 expCtx);

    // All events in a transaction share the same clusterTime, lsid, and txNumber values. If the
    // user wishes to filter out events based on these values, it is possible to rewrite these
    // filters to filter out entire applyOps and commitTransaction entries before they are unwound.
    if (auto rewrittenMatch = change_stream_rewrite::rewriteFilterForFields(
            expCtx, userMatch, {"clusterTime", "lsid", "txnNumber"})) {
        auto transactionFilterWithUserMatch = std::make_unique<AndMatchExpression>();
        transactionFilterWithUserMatch->add(std::move(transactionFilter));
        transactionFilterWithUserMatch->add(std::move(rewrittenMatch));
        return transactionFilterWithUserMatch;
    }

    return transactionFilter;
}

std::unique_ptr<MatchExpression> buildInternalOpFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const MatchExpression* userMatch) {
    // Noop change events:
    //   - reshardBegin: A resharding operation begins.
    //   - reshardDoneCatchUp: "Catch up" phase of reshard operation completes.
    //   - shardCollection: A shardCollection operation has completed.
    std::vector<StringData> internalOpTypes = {
        "reshardBegin"_sd, "reshardDoneCatchUp"_sd, "shardCollection"_sd};

    // Noop change events that are only applicable when merging results on mongoS:
    //   - migrateChunkToNewShard: A chunk migrated to a shard that didn't have any chunks.
    if (expCtx->inMongos || expCtx->needsMerge) {
        internalOpTypes.push_back("migrateChunkToNewShard"_sd);
    }

    // Only return the 'migrateLastChunkFromShard' event if 'showSystemEvents' is set.
    if (expCtx->changeStreamSpec->getShowSystemEvents()) {
        internalOpTypes.push_back("migrateLastChunkFromShard"_sd);
    }

    if (feature_flags::gFeatureFlagChangeStreamsFurtherEnrichedEvents.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        internalOpTypes.push_back("refineCollectionShardKey"_sd);
        internalOpTypes.push_back("reshardCollection"_sd);
    }

    // Build the oplog filter to match the required internal op types.
    BSONArrayBuilder internalOpTypeOrBuilder;
    for (const auto& eventName : internalOpTypes) {
        internalOpTypeOrBuilder.append(BSON("o2." + eventName << BSON("$exists" << true)));
    }

    // TODO SERVER-66138: This filter can be removed after 7.0 release.
    change_stream_legacy::populateInternalOperationFilter(expCtx, &internalOpTypeOrBuilder);

    // Finalize the array of $or filter predicates.
    internalOpTypeOrBuilder.done();

    auto nsRegex = DocumentSourceChangeStream::getNsRegexForChangeStream(expCtx);
    return MatchExpressionParser::parseAndNormalize(BSON("op"
                                                         << "n"
                                                         << "ns" << BSONRegEx(nsRegex) << "$or"
                                                         << internalOpTypeOrBuilder.arr()),
                                                    expCtx);
}

BSONObj getMatchFilterForClassicOperationTypes() {
    return BSON(DocumentSourceChangeStream::kOperationTypeField
                << BSON("$in" << change_stream_legacy::kClassicOperationTypes));
}

}  // namespace change_stream_filter
}  // namespace mongo
