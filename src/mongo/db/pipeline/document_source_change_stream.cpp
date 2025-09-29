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

#include "mongo/db/pipeline/document_source_change_stream.h"

#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/database_name.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/change_stream_pipeline_helpers.h"
#include "mongo/db/pipeline/change_stream_reader_builder.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/pcre_util.h"

#include <string>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;

namespace {
auto& changeStreamsShowExpandedEvents =
    *MetricBuilder<Counter64>{"changeStreams.showExpandedEvents"};
}

// The $changeStream stage is an alias for many stages.
REGISTER_DOCUMENT_SOURCE(changeStream,
                         DocumentSourceChangeStream::LiteParsed::parse,
                         DocumentSourceChangeStream::createFromBson,
                         AllowedWithApiStrict::kConditionally);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamStage, DocumentSourceInternalChangeStreamStage::id)

void DocumentSourceChangeStream::checkValueType(const Value v,
                                                const StringData fieldName,
                                                BSONType expectedType) {
    uassert(40532,
            str::stream() << "Entry field \"" << fieldName << "\" should be "
                          << typeName(expectedType) << ", found: " << typeName(v.getType()),
            (v.getType() == expectedType));
}

void DocumentSourceChangeStream::checkValueTypeOrMissing(const Value v,
                                                         const StringData fieldName,
                                                         BSONType expectedType) {
    if (!v.missing()) {
        checkValueType(v, fieldName, expectedType);
    }
}

StringData DocumentSourceChangeStream::resolveAllCollectionsRegex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // We never expect this method to be called except when building a change stream pipeline.
    tassert(6189300,
            "Expected change stream spec to be set on the expression context",
            expCtx->getChangeStreamSpec());
    // If 'showSystemEvents' is set, return a less stringent regex.
    return (expCtx->getChangeStreamSpec()->getShowSystemEvents()
                ? DocumentSourceChangeStream::kRegexAllCollectionsShowSystemEvents
                : DocumentSourceChangeStream::kRegexAllCollections);
}

std::string DocumentSourceChangeStream::getNsRegexForChangeStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& nss = expCtx->getNamespaceString();
    switch (ChangeStream::getChangeStreamType(nss)) {
        case ChangeStreamType::kCollection:
            // Match the target namespace exactly.
            return fmt::format(
                "^{}$",
                // Change streams will only be enabled in serverless when multitenancy and
                // featureFlag are on, therefore we don't have a tenantid prefix.
                pcre_util::quoteMeta(
                    NamespaceStringUtil::serialize(nss, expCtx->getSerializationContext())));
        case ChangeStreamType::kDatabase:
            // Match all namespaces that start with db name, followed by ".", then NOT followed by
            // '$' or 'system.' unless 'showSystemEvents' is set.
            return fmt::format(
                "^{}\\.{}",
                // Change streams will only be enabled in serverless when multitenancy and
                // featureFlag are on, therefore we don't have a tenantid prefix.
                pcre_util::quoteMeta(
                    DatabaseNameUtil::serialize(nss.dbName(), expCtx->getSerializationContext())),
                resolveAllCollectionsRegex(expCtx));
        case ChangeStreamType::kAllDatabases:
            // Match all namespaces that start with any db name other than admin, config, or local,
            // followed by ".", then NOT '$' or 'system.' unless 'showSystemEvents' is set.
            return fmt::format("{}\\.{}", kRegexAllDBs, resolveAllCollectionsRegex(expCtx));
        default:
            MONGO_UNREACHABLE;
    }
}

BSONObj DocumentSourceChangeStream::getNsMatchObjForChangeStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO SERVER-105554
    // Currently we always return a BSONRegEx with the ns match string. We may optimize this to
    // exact matches ('$eq') for single collection change streams in the future to improve matching
    // performance. This is currently not safe because of collations that can affect the matching.
    return BSON("" << BSONRegEx(getNsRegexForChangeStream(expCtx)));
}

std::string DocumentSourceChangeStream::getViewNsRegexForChangeStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& nss = expCtx->getNamespaceString();
    switch (ChangeStream::getChangeStreamType(nss)) {
        case ChangeStreamType::kDatabase:
            // For a single database, match any events on the system.views collection on that
            // database.
            return fmt::format("^{}\\.system\\.views$",
                               pcre_util::quoteMeta(DatabaseNameUtil::serialize(
                                   nss.dbName(), expCtx->getSerializationContext())));
        case ChangeStreamType::kAllDatabases:
            // Match all system.views collections on all databases.
            return fmt::format("{}\\.system\\.views$", kRegexAllDBs);
        default:
            // We should never attempt to generate this regex for a single-collection stream.
            MONGO_UNREACHABLE_TASSERT(6394400);
    }
}

BSONObj DocumentSourceChangeStream::getViewNsMatchObjForChangeStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO SERVER-105554
    // Currently we always return a BSONRegEx with the view ns match string. We may optimize this to
    // exact matches ('$eq') for single database change streams in the future to improve matching
    // performance. This currently may not be safe because of collations that can affect the
    // matching.
    return BSON("" << BSONRegEx(getViewNsRegexForChangeStream(expCtx)));
}

std::string DocumentSourceChangeStream::getCollRegexForChangeStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& nss = expCtx->getNamespaceString();
    switch (ChangeStream::getChangeStreamType(nss)) {
        case ChangeStreamType::kCollection:
            // Match the target collection exactly.
            return fmt::format("^{}$", pcre_util::quoteMeta(nss.coll()));
        case ChangeStreamType::kDatabase:
        case ChangeStreamType::kAllDatabases:
            // Match any collection; database filtering will be done elsewhere.
            return fmt::format("^{}", resolveAllCollectionsRegex(expCtx));
        default:
            MONGO_UNREACHABLE;
    }
}

BSONObj DocumentSourceChangeStream::getCollMatchObjForChangeStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO SERVER-105554
    // Currently we always return a BSONRegEx with the collection match string. We may optimize this
    // to exact matches ('$eq') for single collection change streams in the future to improve
    // matching performance. This currently may not be safe because of collations that can affect
    // the matching.
    return BSON("" << BSONRegEx(getCollRegexForChangeStream(expCtx)));
}

std::string DocumentSourceChangeStream::getCmdNsRegexForChangeStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& nss = expCtx->getNamespaceString();
    switch (ChangeStream::getChangeStreamType(nss)) {
        case ChangeStreamType::kCollection:
        case ChangeStreamType::kDatabase:
            // Match the target database command namespace exactly.
            return fmt::format("^{}$",
                               pcre_util::quoteMeta(NamespaceStringUtil::serialize(
                                   nss.getCommandNS(), SerializationContext::stateDefault())));
        case ChangeStreamType::kAllDatabases:
            // Match all command namespaces on any database.
            return fmt::format("{}\\.{}", kRegexAllDBs, kRegexCmdColl);
        default:
            MONGO_UNREACHABLE;
    }
}

BSONObj DocumentSourceChangeStream::getCmdNsMatchObjForChangeStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO SERVER-105554
    // Currently we always return a BSONRegEx with the collection-less aggregate ns match string. We
    // may optimize this to exact matches ('$eq') for single collection and single database change
    // streams in the future to improve matching performance. This currently may not be safe because
    // of collations that can affect the matching.
    return BSON("" << BSONRegEx(getCmdNsRegexForChangeStream(expCtx)));
}


Timestamp DocumentSourceChangeStream::getStartTimeForNewStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // If we do not have an explicit starting point, we should start from the latest majority
    // committed operation. If we are on mongoS and do not have a starting point, set it to the
    // current clusterTime so that all shards start in sync.
    auto replCoord = repl::ReplicationCoordinator::get(expCtx->getOperationContext());
    const auto currentTime = !expCtx->getInRouter()
        ? LogicalTime{replCoord->getMyLastAppliedOpTime().getTimestamp()}
        : [&] {
              const auto currentTime = VectorClock::get(expCtx->getOperationContext())->getTime();
              return currentTime.clusterTime();
          }();

    // We always start one tick beyond the most recent operation, to ensure that the stream does not
    // return it.
    return currentTime.addTicks(1).asTimestamp();
}

std::list<intrusive_ptr<DocumentSource>> DocumentSourceChangeStream::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(50808,
            "$changeStream stage expects a document as argument",
            elem.type() == BSONType::object);

    auto spec = DocumentSourceChangeStreamSpec::parse(elem.embeddedObject(),
                                                      IDLParserContext("$changeStream"));

    // Make sure that it is legal to run this $changeStream before proceeding.
    DocumentSourceChangeStream::assertIsLegalSpecification(expCtx, spec);

    // If the user did not specify an explicit starting point, set it to the current time.
    if (!spec.getResumeAfter() && !spec.getStartAfter() && !spec.getStartAtOperationTime()) {
        // Make sure we update the 'startAtOperationTime' in the 'spec' so that we serialize the
        // correct start point when sending it to the shards.
        spec.setStartAtOperationTime(DocumentSourceChangeStream::getStartTimeForNewStream(expCtx));
    }

    // If the stream's default version differs from the client's token version, adopt the higher.
    // This is the token version that will be used once the stream has passed the resume token.
    const auto clientToken = change_stream::resolveResumeTokenFromSpec(expCtx, spec);
    expCtx->setChangeStreamTokenVersion(
        std::max(expCtx->getChangeStreamTokenVersion(), clientToken.version));

    // If the user explicitly requested to resume from a high water mark token, but its version
    // differs from the version chosen above, regenerate it with the new version. There is no need
    // for a resumed HWM stream to adopt the old token version for events at the same clusterTime.
    const bool tokenVersionsDiffer = (clientToken.version != expCtx->getChangeStreamTokenVersion());
    const bool isHighWaterMark = ResumeToken::isHighWaterMarkToken(clientToken);
    if (isHighWaterMark && tokenVersionsDiffer && (spec.getResumeAfter() || spec.getStartAfter())) {
        spec.setResumeAfter(ResumeToken(ResumeToken::makeHighWaterMarkToken(
            clientToken.clusterTime, expCtx->getChangeStreamTokenVersion())));
        spec.setStartAfter(boost::none);
    }

    // Obtain the resume token from the spec. This will be used when building the pipeline.
    auto resumeToken = change_stream::resolveResumeTokenFromSpec(expCtx, spec);

    const auto& nss = expCtx->getNamespaceString();

    ChangeStream changeStream(
        fromIgnoreRemovedShardsParameter(static_cast<bool>(spec.getIgnoreRemovedShards())),
        ChangeStream::getChangeStreamType(nss),
        nss);

    ChangeStreamReaderVersionEnum changeStreamVersion =
        _determineChangeStreamReaderVersion(expCtx, resumeToken.clusterTime, spec, changeStream);

    // Override global change stream reader version with the version just determined.
    spec.setVersion(changeStreamVersion);

    if (changeStreamVersion == ChangeStreamReaderVersionEnum::kV2) {
        OperationContext* opCtx = expCtx->getOperationContext();
        ChangeStreamReaderBuilder* readerBuilder =
            ChangeStreamReaderBuilder::get(opCtx->getServiceContext());
        tassert(10743908, "expecting ChangeStreamReaderBuilder to be available", readerBuilder);

        // Set supported control events for the 'DocumentSourceChangeStreamTransform' stage. That
        // stage will pick up the supported events from the change stream definition in the
        // ExpressionContext later.
        auto controlEventTypes =
            readerBuilder->getControlEventTypesOnDataShard(opCtx, changeStream);

        std::vector<std::string> supportedEvents(controlEventTypes.begin(),
                                                 controlEventTypes.end());
        spec.setSupportedEvents(std::move(supportedEvents));
    }

    // Save a copy of the spec on the expression context. Used when building the oplog filter.
    expCtx->setChangeStreamSpec(spec);

    changeStreamsShowExpandedEvents.increment(spec.getShowExpandedEvents());

    return change_stream::pipeline_helpers::buildPipeline(expCtx, spec, resumeToken);
}

ChangeStreamReaderVersionEnum DocumentSourceChangeStream::_determineChangeStreamReaderVersion(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp atClusterTime,
    const DocumentSourceChangeStreamSpec& spec,
    const ChangeStream& changeStream) try {

    if (!expCtx->getInRouter()) {
        // If we are not on a router, we always set reader version v1.
        return ChangeStreamReaderVersionEnum::kV1;
    }

    OperationContext* opCtx = expCtx->getOperationContext();

    // Check feature flag 'featureFlagChangeStreamPreciseShardTargeting' that is required to enable
    // v2 change stream readers.
    if (!feature_flags::gFeatureFlagChangeStreamPreciseShardTargeting.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // Feature flag is disabled. Default to v1.
        return ChangeStreamReaderVersionEnum::kV1;
    }

    // Check If the user explicitly requested a specific change stream reader version.
    const auto& version = spec.getVersion();

    // If no change stream reader version was explicitly selected, or if it was set explicitly to
    // v1, change stream reader version v1 will be used.
    if (!version.has_value() || *version == ChangeStreamReaderVersionEnum::kV1) {
        return ChangeStreamReaderVersionEnum::kV1;
    }

    // The user has explicitly selected the v2 change stream reader version.

    // v2 change stream readers are currently only supported for collection-level change streams.
    if (changeStream.getChangeStreamType() != ChangeStreamType::kCollection) {
        return ChangeStreamReaderVersionEnum::kV1;
    }

    ChangeStreamReaderBuilder* readerBuilder =
        ChangeStreamReaderBuilder::get(opCtx->getServiceContext());

    tassert(10743904, "expecting ChangeStreamReaderBuilder to be available", readerBuilder);

    DataToShardsAllocationQueryService* dataToShardsAllocationQueryService =
        DataToShardsAllocationQueryService::get(opCtx);

    tassert(10743906,
            "expecting DataToShardsAllocationQueryService to be available",
            dataToShardsAllocationQueryService);

    if (dataToShardsAllocationQueryService->getAllocationToShardsStatus(opCtx, atClusterTime) ==
        AllocationToShardsStatus::kNotAvailable) {
        // No shard placement information is available. Use v1 change stream reader.
        return ChangeStreamReaderVersionEnum::kV1;
    }

    // All requirements for using a v2 change stream reader are satisfied.
    return ChangeStreamReaderVersionEnum::kV2;
} catch (const DBException& ex) {
    // Log any error that we have caught while determining the change stream reader version.
    LOGV2_DEBUG(10743907,
                3,
                "caught exception while determining change stream reader version",
                "error"_attr = ex.toStatus());
    throw;
}

void DocumentSourceChangeStream::assertIsLegalSpecification(
    const intrusive_ptr<ExpressionContext>& expCtx, const DocumentSourceChangeStreamSpec& spec) {
    // We can only run on a replica set, or through mongoS. Confirm that this is the case.
    auto replCoord = repl::ReplicationCoordinator::get(expCtx->getOperationContext());
    uassert(40573,
            "The $changeStream stage is only supported on replica sets",
            expCtx->getInRouter() || (replCoord && replCoord->getSettings().isReplSet()));

    // We will not validate user specified options when we are not expecting to execute queries,
    // such as during $queryStats.
    if (!expCtx->getMongoProcessInterface()->isExpectedToExecuteQueries()) {
        return;
    }

    // If 'allChangesForCluster' is true, the stream must be opened on the 'admin' database with
    // {aggregate: 1}.
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "A $changeStream with 'allChangesForCluster:true' may only be opened "
                             "on the 'admin' database, and with no collection name; found "
                          << expCtx->getNamespaceString().toStringForErrorMsg(),
            !spec.getAllChangesForCluster() ||
                (expCtx->getNamespaceString().isAdminDB() &&
                 expCtx->getNamespaceString().isCollectionlessAggregateNS()));

    // Prevent $changeStream from running on internal databases. A stream may run against the
    // 'admin' database iff 'allChangesForCluster' is true. A stream may run against the 'config'
    // database iff 'allowToRunOnConfigDB' is true.
    const bool isNotBannedInternalDB = !expCtx->getNamespaceString().isLocalDB() &&
        (!expCtx->getNamespaceString().isConfigDB() || spec.getAllowToRunOnConfigDB());
    uassert(
        ErrorCodes::InvalidNamespace,
        str::stream() << "$changeStream may not be opened on the internal "
                      << expCtx->getNamespaceString().dbName().toStringForErrorMsg() << " database",
        expCtx->getNamespaceString().isAdminDB() ? static_cast<bool>(spec.getAllChangesForCluster())
                                                 : isNotBannedInternalDB);

    // Prevent $changeStream from running on internal collections in any database. A stream may run
    // against the internal collections iff 'allowToRunOnSystemNS' is true and the stream is not
    // opened through a mongos process.
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "$changeStream may not be opened on the internal "
                          << expCtx->getNamespaceString().toStringForErrorMsg() << " collection"
                          << (spec.getAllowToRunOnSystemNS() ? " through router" : ""),
            !expCtx->getNamespaceString().isSystem() ||
                (spec.getAllowToRunOnSystemNS() && !expCtx->getInRouter()));

    uassert(31123,
            "Change streams from router may not show migration events",
            !(expCtx->getInRouter() && spec.getShowMigrationEvents()));

    uassert(50865,
            "Do not specify both 'resumeAfter' and 'startAfter' in a $changeStream stage",
            !spec.getResumeAfter() || !spec.getStartAfter());

    auto resumeToken = (spec.getResumeAfter() || spec.getStartAfter())
        ? change_stream::resolveResumeTokenFromSpec(expCtx, spec)
        : boost::optional<ResumeTokenData>();

    uassert(40674,
            "Only one type of resume option is allowed, but multiple were found",
            !(spec.getStartAtOperationTime() && resumeToken));

    uassert(ErrorCodes::InvalidResumeToken,
            "Attempting to resume a change stream using 'resumeAfter' is not allowed from an "
            "invalidate notification",
            !(spec.getResumeAfter() && resumeToken->fromInvalidate));

    // If we are resuming a single-collection stream, the resume token should always contain a
    // UUID unless the token is from endOfTransaction event or a high water mark.
    uassert(ErrorCodes::InvalidResumeToken,
            "Attempted to resume a single-collection stream, but the resume token does not "
            "include a UUID",
            !resumeToken || resumeToken->uuid || !expCtx->isSingleNamespaceAggregation() ||
                ResumeToken::isHighWaterMarkToken(*resumeToken) ||
                Value::compare(resumeToken->eventIdentifier["operationType"],
                               Value("endOfTransaction"_sd),
                               nullptr) == 0);
}

}  // namespace mongo
