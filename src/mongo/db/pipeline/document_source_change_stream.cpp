
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/db/pipeline/document_source_change_stream.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_change_stream_close_cursor.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_check_invalidate.h"
#include "mongo/db/pipeline/document_source_check_resume_token.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_watch_for_uuid.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using std::string;
using std::vector;

// The $changeStream stage is an alias for many stages, but we need to be able to serialize
// and re-parse the pipeline. To make this work, the 'transformation' stage will serialize itself
// with the original specification, and all other stages that are created during the alias expansion
// will not serialize themselves.
REGISTER_MULTI_STAGE_ALIAS(changeStream,
                           DocumentSourceChangeStream::LiteParsed::parse,
                           DocumentSourceChangeStream::createFromBson);

constexpr StringData DocumentSourceChangeStream::kDocumentKeyField;
constexpr StringData DocumentSourceChangeStream::kFullDocumentField;
constexpr StringData DocumentSourceChangeStream::kIdField;
constexpr StringData DocumentSourceChangeStream::kNamespaceField;
constexpr StringData DocumentSourceChangeStream::kUuidField;
constexpr StringData DocumentSourceChangeStream::kUpdateDescriptionField;
constexpr StringData DocumentSourceChangeStream::kOperationTypeField;
constexpr StringData DocumentSourceChangeStream::kStageName;
constexpr StringData DocumentSourceChangeStream::kClusterTimeField;
constexpr StringData DocumentSourceChangeStream::kTxnNumberField;
constexpr StringData DocumentSourceChangeStream::kLsidField;
constexpr StringData DocumentSourceChangeStream::kRenameTargetNssField;
constexpr StringData DocumentSourceChangeStream::kUpdateOpType;
constexpr StringData DocumentSourceChangeStream::kDeleteOpType;
constexpr StringData DocumentSourceChangeStream::kReplaceOpType;
constexpr StringData DocumentSourceChangeStream::kInsertOpType;
constexpr StringData DocumentSourceChangeStream::kDropCollectionOpType;
constexpr StringData DocumentSourceChangeStream::kRenameCollectionOpType;
constexpr StringData DocumentSourceChangeStream::kDropDatabaseOpType;
constexpr StringData DocumentSourceChangeStream::kInvalidateOpType;
constexpr StringData DocumentSourceChangeStream::kNewShardDetectedOpType;

constexpr StringData DocumentSourceChangeStream::kRegexAllCollections;
constexpr StringData DocumentSourceChangeStream::kRegexAllDBs;
constexpr StringData DocumentSourceChangeStream::kRegexCmdColl;

namespace {

static constexpr StringData kOplogMatchExplainName = "$_internalOplogMatch"_sd;
}  // namespace

intrusive_ptr<DocumentSourceOplogMatch> DocumentSourceOplogMatch::create(
    BSONObj filter, const intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceOplogMatch(std::move(filter), expCtx);
}

const char* DocumentSourceOplogMatch::getSourceName() const {
    // This is used in error reporting, particularly if we find this stage in a position other
    // than first, so report the name as $changeStream.
    return DocumentSourceChangeStream::kStageName.rawData();
}

DocumentSource::StageConstraints DocumentSourceOplogMatch::constraints(
    Pipeline::SplitState pipeState) const {

    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kFirst,
                                 HostTypeRequirement::kAnyShard,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);
    constraints.isIndependentOfAnyCollection =
        pExpCtx->ns.isCollectionlessAggregateNS() ? true : false;
    return constraints;
}

/**
 * Only serialize this stage for explain purposes, otherwise keep it hidden so that we can
 * properly alias.
 */
Value DocumentSourceOplogMatch::serialize(optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        return Value(Document{{kOplogMatchExplainName, Document{}}});
    }
    return Value();
}

DocumentSourceOplogMatch::DocumentSourceOplogMatch(BSONObj filter,
                                                   const intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceMatch(std::move(filter), expCtx) {}

void DocumentSourceChangeStream::checkValueType(const Value v,
                                                const StringData filedName,
                                                BSONType expectedType) {
    uassert(40532,
            str::stream() << "Entry field \"" << filedName << "\" should be "
                          << typeName(expectedType)
                          << ", found: "
                          << typeName(v.getType()),
            (v.getType() == expectedType));
}

//
// Helpers for building the oplog filter.
//
namespace {

/**
 * Constructs the filter which will match 'applyOps' oplog entries that are:
 * 1) Part of a transaction
 * 2) Have sub-entries which should be returned in the change stream
 */
BSONObj getTxnApplyOpsFilter(BSONElement nsMatch, const NamespaceString& nss) {
    BSONObjBuilder applyOpsBuilder;
    applyOpsBuilder.append("op", "c");
    applyOpsBuilder.append("lsid", BSON("$exists" << true));
    applyOpsBuilder.append("txnNumber", BSON("$exists" << true));
    const std::string& kApplyOpsNs = "o.applyOps.ns";
    applyOpsBuilder.appendAs(nsMatch, kApplyOpsNs);
    return applyOpsBuilder.obj();
}
}  // namespace

DocumentSourceChangeStream::ChangeStreamType DocumentSourceChangeStream::getChangeStreamType(
    const NamespaceString& nss) {

    // If we have been permitted to run on admin, 'allChangesForCluster' must be true.
    return (nss.isAdminDB()
                ? ChangeStreamType::kAllChangesForCluster
                : (nss.isCollectionlessAggregateNS() ? ChangeStreamType::kSingleDatabase
                                                     : ChangeStreamType::kSingleCollection));
}

std::string DocumentSourceChangeStream::getNsRegexForChangeStream(const NamespaceString& nss) {
    auto type = getChangeStreamType(nss);
    switch (type) {
        case ChangeStreamType::kSingleCollection:
            // Match the target namespace exactly.
            return "^" + nss.ns() + "$";
        case ChangeStreamType::kSingleDatabase:
            // Match all namespaces that start with db name, followed by ".", then NOT followed by
            // '$' or 'system.'
            return "^" + nss.db() + "\\." + kRegexAllCollections;
        case ChangeStreamType::kAllChangesForCluster:
            // Match all namespaces that start with any db name other than admin, config, or local,
            // followed by ".", then NOT followed by '$' or 'system.'.
            return kRegexAllDBs + "\\." + kRegexAllCollections;
        default:
            MONGO_UNREACHABLE;
    }
}


BSONObj DocumentSourceChangeStream::buildMatchFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp startFrom,
    bool startFromInclusive) {
    auto nss = expCtx->ns;

    ChangeStreamType sourceType = getChangeStreamType(nss);

    // 1) Supported commands that have the target db namespace (e.g. test.$cmd) in "ns" field.
    BSONArrayBuilder relevantCommands;

    if (sourceType == ChangeStreamType::kSingleCollection) {
        relevantCommands.append(BSON("o.drop" << nss.coll()));
        // Generate 'rename' entries if the change stream is open on the source or target namespace.
        relevantCommands.append(BSON("o.renameCollection" << nss.ns()));
        relevantCommands.append(BSON("o.to" << nss.ns()));
        if (expCtx->collation.isEmpty()) {
            // If the user did not specify a collation, they should be using the collection's
            // default collation. So a "create" command which has any collation present would
            // invalidate the change stream, since that must mean the stream was created before the
            // collection existed and used the simple collation, which is no longer the default.
            relevantCommands.append(
                BSON("o.create" << nss.coll() << "o.collation" << BSON("$exists" << true)));
        }
    } else {
        // For change streams on an entire database, include notifications for individual collection
        // drops and renames which will not invalidate the stream. Also include the 'dropDatabase'
        // command which will invalidate the stream.
        relevantCommands.append(BSON("o.drop" << BSONRegEx("^" + kRegexAllCollections)));
        relevantCommands.append(BSON("o.dropDatabase" << BSON("$exists" << true)));
        relevantCommands.append(
            BSON("o.renameCollection" << BSONRegEx(getNsRegexForChangeStream(nss))));
    }

    // For cluster-wide $changeStream, match the command namespace of any database other than admin,
    // config, or local. Otherwise, match only against the target db's command namespace.
    auto cmdNsFilter = (sourceType == ChangeStreamType::kAllChangesForCluster
                            ? BSON("ns" << BSONRegEx(kRegexAllDBs + "\\." + kRegexCmdColl))
                            : BSON("ns" << nss.getCommandNS().ns()));

    // 1.1) Commands that are on target db(s) and one of the above supported commands.
    auto commandsOnTargetDb =
        BSON("$and" << BSON_ARRAY(cmdNsFilter << BSON("$or" << relevantCommands.arr())));

    // 1.2) Supported commands that have arbitrary db namespaces in "ns" field.
    auto renameDropTarget = BSON("o.to" << BSONRegEx(getNsRegexForChangeStream(nss)));

    // All supported commands that are either (1.1) or (1.2).
    BSONObj commandMatch = BSON("op"
                                << "c"
                                << OR(commandsOnTargetDb, renameDropTarget));

    // 2.1) Normal CRUD ops.
    auto normalOpTypeMatch = BSON("op" << NE << "n");

    // 2.2) A chunk gets migrated to a new shard that doesn't have any chunks.
    auto chunkMigratedMatch = BSON("op"
                                   << "n"
                                   << "o2.type"
                                   << "migrateChunkToNewShard");

    // 2) Supported operations on the target namespace.
    BSONObj nsMatch = BSON("ns" << BSONRegEx(getNsRegexForChangeStream(nss)));
    auto opMatch = BSON(nsMatch["ns"] << OR(normalOpTypeMatch, chunkMigratedMatch));

    // 3) Look for 'applyOps' which were created as part of a transaction.
    BSONObj applyOps = getTxnApplyOpsFilter(nsMatch["ns"], nss);

    // 4) Look for changes to the feature compatibility version. These show up as updates to the
    // admin.system.version collection.
    BSONObj fcvChange = BSON(
        "$and" << BSON_ARRAY(
            BSON("ns" << NamespaceString::kServerConfigurationNamespace.ns())
            // Ignore entries which correspond to the beginning of a transition. We only need to
            // care about those which actually commit a change to the feature compatibility version.
            << BSON("o.targetVersion" << BSON("$exists" << false))
            << BSON("o.version" << BSON("$exists" << true))));

    // Match oplog entries after "start" and are either supported (1) commands or (2) operations,
    // excepting those tagged "fromMigrate". Include the resume token, if resuming, so we can verify
    // it was still present in the oplog.
    return BSON("$and" << BSON_ARRAY(BSON("ts" << (startFromInclusive ? GTE : GT) << startFrom)
                                     << BSON(OR(opMatch, commandMatch, applyOps, fcvChange))
                                     << BSON("fromMigrate" << NE << true)));
}

namespace {

/**
 * Throws an assertion if this pipeline might need to use a collation but it can't figure out what
 * the collation should be. Specifically, it is only safe to resume if at least one of the following
 * is true:
 *      * The request has an explicit collation set, so we don't need to know if there was a default
 *        collation on the collection.
 *      * The request is 'collectionless', meaning it's a change stream on a whole database or a
 *        whole cluster. Unlike individual collections, there is no concept of a default collation
 *        at the level of an entire database or cluster.
 *      * The resume token contains a UUID and a collection with that UUID still exists, thus we can
 *        figure out its default collation.
 */
void assertResumeAllowed(const intrusive_ptr<ExpressionContext>& expCtx,
                         const ResumeTokenData& tokenData) {
    if (!expCtx->collation.isEmpty()) {
        // Explicit collation has been set, it's okay to resume.
        return;
    }

    if (!expCtx->isSingleNamespaceAggregation()) {
        // Change stream on a whole database or cluster, do not need to worry about collation.
        return;
    }

    if (!tokenData.uuid && ResumeToken::isHighWaterMarkToken(tokenData)) {
        // The only time we see a single-collection high water mark with no UUID is when the stream
        // was opened on a non-existent collection. We allow this to proceed, as the resumed stream
        // will immediately invalidate itself if it observes a createCollection event in the oplog
        // with a non-simple collation.
        return;
    }

    const auto cannotResumeErrMsg =
        "Attempted to resume a stream on a collection which has been dropped. The change stream's "
        "pipeline may need to make comparisons which should respect the collection's default "
        "collation, which can no longer be determined.";
    // Verify that the UUID on the expression context matches the UUID in the resume token.
    // TODO SERVER-35254: If we're on a stale mongos, this check may incorrectly reject a valid
    // resume token since the UUID on the expression context could be for a previous version of the
    // collection.
    uassert(ErrorCodes::InvalidResumeToken,
            cannotResumeErrMsg,
            expCtx->uuid && tokenData.uuid && expCtx->uuid.get() == tokenData.uuid.get());
}

intrusive_ptr<DocumentSource> createTransformationStage(
    const intrusive_ptr<ExpressionContext>& expCtx,
    BSONObj changeStreamSpec,
    ServerGlobalParams::FeatureCompatibility::Version fcv) {
    // Mark the transformation stage as independent of any collection if the change stream is
    // watching all collections in the database.
    const bool isIndependentOfAnyCollection = expCtx->ns.isCollectionlessAggregateNS();
    return intrusive_ptr<DocumentSource>(new DocumentSourceChangeStreamTransform(
        expCtx, changeStreamSpec, fcv, isIndependentOfAnyCollection));
}

list<intrusive_ptr<DocumentSource>> buildPipeline(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec spec,
    ServerGlobalParams::FeatureCompatibility::Version fcv,
    BSONElement elem) {
    list<intrusive_ptr<DocumentSource>> stages;
    boost::optional<Timestamp> startFrom;
    intrusive_ptr<DocumentSource> resumeStage = nullptr;

    if (auto resumeAfter = spec.getResumeAfter()) {
        ResumeToken token = resumeAfter.get();
        ResumeTokenData tokenData = token.getData();

        // Cannot resume a change stream using a 'fromInvalidate' resume token.
        uassert(ErrorCodes::InvalidResumeToken,
                "Attempting to resume a change stream using 'resumeAfter' is not allowed from an "
                "invalidate notification.",
                !tokenData.fromInvalidate);

        // Verify that the requested resume attempt is possible based on the stream type, resume
        // token UUID, and collation.
        assertResumeAllowed(expCtx, tokenData);

        // Store the resume token as the initial postBatchResumeToken for this stream.
        expCtx->initialPostBatchResumeToken =
            token.toDocument(ResumeToken::SerializationFormat::kHexString).toBson();

        // For a regular resume token, we must ensure that (1) all shards are capable of resuming
        // from the given clusterTime, and (2) that we observe the resume token event in the stream
        // before any event that would sort after it. High water mark tokens, however, do not refer
        // to a specific event; we thus only need to check (1), similar to 'startAtOperationTime'.
        startFrom = tokenData.clusterTime;
        if (expCtx->needsMerge || ResumeToken::isHighWaterMarkToken(tokenData)) {
            resumeStage = DocumentSourceShardCheckResumability::create(expCtx, tokenData);
        } else {
            resumeStage = DocumentSourceEnsureResumeTokenPresent::create(expCtx, tokenData);
        }
    }

    auto resumeAfterClusterTime = spec.getResumeAfterClusterTimeDeprecated();
    auto startAtOperationTime = spec.getStartAtOperationTime();

    uassert(40674,
            "Only one type of resume option is allowed, but multiple were found.",
            !resumeStage || (!resumeAfterClusterTime && !startAtOperationTime));

    if (resumeAfterClusterTime) {
        if (fcv >= ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
            warning() << "The '$_resumeAfterClusterTime' option is deprecated, please use "
                         "'startAtOperationTime' instead.";
        }
        startFrom = resumeAfterClusterTime->getTimestamp();
    }

    // New field name starting in 4.0 is 'startAtOperationTime'.
    if (startAtOperationTime) {
        uassert(50573,
                str::stream()
                    << "Do not specify both "
                    << DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName
                    << " and "
                    << DocumentSourceChangeStreamSpec::kResumeAfterClusterTimeDeprecatedFieldName
                    << " in a $changeStream stage.",
                !resumeAfterClusterTime);
        startFrom = *startAtOperationTime;
        resumeStage = DocumentSourceShardCheckResumability::create(expCtx, *startFrom);
    }

    // There might not be a starting point if we're on mongos, otherwise we should either have a
    // 'resumeAfter' starting point, or should start from the latest majority committed operation.
    auto replCoord = repl::ReplicationCoordinator::get(expCtx->opCtx);
    uassert(40573,
            "The $changeStream stage is only supported on replica sets",
            expCtx->inMongos || (replCoord &&
                                 replCoord->getReplicationMode() ==
                                     repl::ReplicationCoordinator::Mode::modeReplSet));
    if (!startFrom && !expCtx->inMongos) {
        startFrom = replCoord->getMyLastAppliedOpTime().getTimestamp();
    }

    if (startFrom) {
        const bool startFromInclusive = (resumeStage != nullptr);
        stages.push_back(DocumentSourceOplogMatch::create(
            DocumentSourceChangeStream::buildMatchFilter(expCtx, *startFrom, startFromInclusive),
            expCtx));

        // If we haven't already populated the initial PBRT, then we are starting from a specific
        // timestamp rather than a resume token. Initialize the PBRT to a high water mark token.
        if (expCtx->initialPostBatchResumeToken.isEmpty()) {
            Timestamp startTime{startFrom->getSecs(), startFrom->getInc() + (!startFromInclusive)};
            const auto resumeToken = ResumeToken::makeHighWaterMarkToken(startTime, expCtx->uuid);
            expCtx->initialPostBatchResumeToken =
                resumeToken.toDocument(ResumeToken::SerializationFormat::kHexString).toBson();
        }
    }

    stages.push_back(createTransformationStage(expCtx, elem.embeddedObject(), fcv));

    // If this is a single-collection stream but we don't have a UUID set on the expression context,
    // then the stream was opened before the collection exists. Add a stage which will populate the
    // UUID using the first change stream result observed by the pipeline during execution.
    if (!expCtx->uuid && expCtx->isSingleNamespaceAggregation()) {
        stages.push_back(DocumentSourceWatchForUUID::create(expCtx));
    }

    // The resume stage must come after the check invalidate stage so that the former can determine
    // whether the event that matches the resume token should be followed by an "invalidate" event.
    stages.push_back(DocumentSourceCheckInvalidate::create(expCtx));
    if (resumeStage) {
        stages.push_back(resumeStage);
    }

    return stages;
}

}  // namespace

list<intrusive_ptr<DocumentSource>> DocumentSourceChangeStream::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(50808,
            "$changeStream stage expects a document as argument.",
            elem.type() == BSONType::Object);

    // A change stream is a tailable + awaitData cursor.
    expCtx->tailableMode = TailableModeEnum::kTailableAndAwaitData;

    auto spec = DocumentSourceChangeStreamSpec::parse(IDLParserErrorContext("$changeStream"),
                                                      elem.embeddedObject());

    const auto fcv = serverGlobalParams.featureCompatibility.getVersion();
    // Make sure that it is legal to run this $changeStream before proceeding.
    DocumentSourceChangeStream::assertIsLegalSpecification(expCtx, spec, fcv);

    auto fullDocOption = spec.getFullDocument();
    uassert(40575,
            str::stream() << "unrecognized value for the 'fullDocument' option to the "
                             "$changeStream stage. Expected \"default\" or "
                             "\"updateLookup\", got \""
                          << fullDocOption
                          << "\"",
            fullDocOption == "updateLookup"_sd || fullDocOption == "default"_sd);

    const bool shouldLookupPostImage = (fullDocOption == "updateLookup"_sd);

    auto stages = buildPipeline(expCtx, spec, fcv, elem);
    if (!expCtx->needsMerge) {
        // There should only be one close cursor stage. If we're on the shards and producing input
        // to be merged, do not add a close cursor stage, since the mongos will already have one.
        stages.push_back(DocumentSourceCloseCursor::create(expCtx));

        // If this is a single-collection stream but we do not have a UUID set on the expression
        // context, then the stream was opened before the collection exists. Add a stage on mongoS
        // which will watch for and populate the UUID using the first result seen by the pipeline.
        if (expCtx->inMongos && !expCtx->uuid && expCtx->isSingleNamespaceAggregation()) {
            stages.push_back(DocumentSourceWatchForUUID::create(expCtx));
        }

        // There should be only one post-image lookup stage.  If we're on the shards and producing
        // input to be merged, the lookup is done on the mongos.
        if (shouldLookupPostImage) {
            stages.push_back(DocumentSourceLookupChangePostImage::create(expCtx));
        }
    }
    return stages;
}

BSONObj DocumentSourceChangeStream::replaceResumeTokenInCommand(const BSONObj originalCmdObj,
                                                                const BSONObj resumeToken) {
    Document originalCmd(originalCmdObj);
    auto pipeline = originalCmd[AggregationRequest::kPipelineName].getArray();
    // A $changeStream must be the first element of the pipeline in order to be able
    // to replace (or add) a resume token.
    invariant(!pipeline[0][DocumentSourceChangeStream::kStageName].missing());

    MutableDocument changeStreamStage(
        pipeline[0][DocumentSourceChangeStream::kStageName].getDocument());
    changeStreamStage[DocumentSourceChangeStreamSpec::kResumeAfterFieldName] = Value(resumeToken);

    // If the command was initially specified with a startAtOperationTime, we need to remove it
    // to use the new resume token.
    changeStreamStage[DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName] = Value();
    pipeline[0] =
        Value(Document{{DocumentSourceChangeStream::kStageName, changeStreamStage.freeze()}});
    MutableDocument newCmd(originalCmd);
    newCmd[AggregationRequest::kPipelineName] = Value(pipeline);
    return newCmd.freeze().toBson();
}

void DocumentSourceChangeStream::assertIsLegalSpecification(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec,
    ServerGlobalParams::FeatureCompatibility::Version fcv) {
    // Change stream on an entire database is a new 4.0 feature.
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            str::stream() << "$changeStream on an entire database is not allowed in the current "
                             "feature compatibility version. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << " for more information.",
            !expCtx->ns.isCollectionlessAggregateNS() ||
                fcv >= ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);

    // If 'allChangesForCluster' is true, the stream must be opened on the 'admin' database with
    // {aggregate: 1}.
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "A $changeStream with 'allChangesForCluster:true' may only be opened "
                             "on the 'admin' database, and with no collection name; found "
                          << expCtx->ns.ns(),
            !spec.getAllChangesForCluster() ||
                (expCtx->ns.isAdminDB() && expCtx->ns.isCollectionlessAggregateNS()));

    // Prevent $changeStream from running on internal databases. A stream may run against the
    // 'admin' database iff 'allChangesForCluster' is true.
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "$changeStream may not be opened on the internal " << expCtx->ns.db()
                          << " database",
            expCtx->ns.isAdminDB() ? spec.getAllChangesForCluster()
                                   : (!expCtx->ns.isLocal() && !expCtx->ns.isConfigDB()));

    // Prevent $changeStream from running on internal collections in any database.
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "$changeStream may not be opened on the internal " << expCtx->ns.ns()
                          << " collection",
            !expCtx->ns.isSystem());
}

}  // namespace mongo
