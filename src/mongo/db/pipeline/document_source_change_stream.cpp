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
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_knobs_gen.h"
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

StageConstraints DocumentSourceOplogMatch::constraints(Pipeline::SplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kFirst,
                                 HostTypeRequirement::kAnyShard,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
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
                          << typeName(expectedType) << ", found: " << typeName(v.getType()),
            (v.getType() == expectedType));
}

//
// Helpers for building the oplog filter.
//
namespace {

/**
 * Constructs a filter matching any 'applyOps' commands that commit a transaction. An 'applyOps'
 * command implicitly commits a transaction if _both_ of the following are true:
 * 1) it is not marked with the 'partialTxn' field, which would indicate that there are more entries
 *    to come in the transaction and
 * 2) it is not marked with the 'prepare' field, which would indicate that the transaction is only
 *    committed if there is a follow-up 'commitTransaction' command in the oplog.
 *
 * This filter will ignore all but the last 'applyOps' command in a transaction comprising multiple
 * 'applyOps' commands, and it will ignore all 'applyOps' commands in a prepared transaction. The
 * change stream traverses back through the oplog to recover the ignored commands when it sees an
 * entry that commits a transaction.
 *
 * As an optimization, this filter also ignores any transaction with just a single 'applyOps' if
 * that 'applyOps' does not contain any updates that modify the namespace that the change stream is
 * watching.
 */
BSONObj getTxnApplyOpsFilter(BSONElement nsMatch, const NamespaceString& nss) {
    BSONObjBuilder applyOpsBuilder;
    applyOpsBuilder.append("op", "c");

    // "o.applyOps" must be an array with at least one element
    applyOpsBuilder.append("o.applyOps.0", BSON("$exists" << true));
    applyOpsBuilder.append("lsid", BSON("$exists" << true));
    applyOpsBuilder.append("txnNumber", BSON("$exists" << true));
    applyOpsBuilder.append("o.prepare", BSON("$not" << BSON("$eq" << true)));
    applyOpsBuilder.append("o.partialTxn", BSON("$not" << BSON("$eq" << true)));
    {
        // Include this 'applyOps' if it has an operation with a matching namespace _or_ if it has a
        // 'prevOpTime' link to another 'applyOps' command, indicating a multi-entry transaction.
        BSONArrayBuilder orBuilder(applyOpsBuilder.subarrayStart("$or"));
        {
            {
                BSONObjBuilder nsMatchBuilder(orBuilder.subobjStart());
                nsMatchBuilder.appendAs(nsMatch, "o.applyOps.ns"_sd);
            }
            // The default repl::OpTime is the value used to indicate a null "prevOpTime" link.
            orBuilder.append(BSON(repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName
                                  << BSON("$ne" << repl::OpTime().toBSON())));
        }
    }
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
    auto regexEscape = [](const std::string& source) {
        std::string result = "";
        std::string escapes = "*+|()^?[]./\\$";
        for (const char& c : source) {
            if (escapes.find(c) != std::string::npos) {
                result.append("\\");
            }
            result += c;
        }
        return result;
    };

    auto type = getChangeStreamType(nss);
    switch (type) {
        case ChangeStreamType::kSingleCollection:
            // Match the target namespace exactly.
            return "^" + regexEscape(nss.ns()) + "$";
        case ChangeStreamType::kSingleDatabase:
            // Match all namespaces that start with db name, followed by ".", then NOT followed by
            // '$' or 'system.'
            return "^" + regexEscape(nss.db().toString()) + "\\." + kRegexAllCollections;
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
    Timestamp startFromInclusive,
    bool showMigrationEvents) {
    auto nss = expCtx->ns;

    ChangeStreamType sourceType = getChangeStreamType(nss);

    // 1) Supported commands that have the target db namespace (e.g. test.$cmd) in "ns" field.
    BSONArrayBuilder relevantCommands;

    if (sourceType == ChangeStreamType::kSingleCollection) {
        relevantCommands.append(BSON("o.drop" << nss.coll()));
        // Generate 'rename' entries if the change stream is open on the source or target namespace.
        relevantCommands.append(BSON("o.renameCollection" << nss.ns()));
        relevantCommands.append(BSON("o.to" << nss.ns()));
    } else {
        // For change streams on an entire database, include notifications for drops and renames of
        // non-system collections which will not invalidate the stream. Also include the
        // 'dropDatabase' command which will invalidate the stream.
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

    // 1.3) Transaction commit commands.
    auto transactionCommit = BSON("o.commitTransaction" << 1);

    // All supported commands that are either (1.1), (1.2) or (1.3).
    BSONObj commandMatch = BSON("op"
                                << "c"
                                << OR(commandsOnTargetDb, renameDropTarget, transactionCommit));

    // 2) Supported operations on the operation namespace, optionally including those from
    // migrations.
    BSONObj opNsMatch = BSON("ns" << BSONRegEx(getNsRegexForChangeStream(nss)));

    // 2.1) Normal CRUD ops.
    auto normalOpTypeMatch = BSON("op" << NE << "n");

    // TODO SERVER-44039: we continue to generate 'kNewShardDetected' events for compatibility
    // with 4.2, even though we no longer rely on them to detect new shards. We may wish to remove
    // this mechanism in 4.6, or retain it for future cases where a change stream is targeted to a
    // subset of shards. See SERVER-44039 for details.

    // 2.2) A chunk gets migrated to a new shard that doesn't have any chunks.
    auto chunkMigratedNewShardMatch = BSON("op"
                                           << "n"
                                           << "o2.type"
                                           << "migrateChunkToNewShard");

    // Supported operations that are either (2.1) or (2.2).
    BSONObj normalOrChunkMigratedMatch =
        BSON(opNsMatch["ns"] << OR(normalOpTypeMatch, chunkMigratedNewShardMatch));

    // Filter excluding entries resulting from chunk migration.
    BSONObj notFromMigrateFilter = BSON("fromMigrate" << NE << true);

    BSONObj opMatch =
        (showMigrationEvents
             ? normalOrChunkMigratedMatch
             : BSON("$and" << BSON_ARRAY(normalOrChunkMigratedMatch << notFromMigrateFilter)));

    // 3) Look for 'applyOps' which were created as part of a transaction.
    BSONObj applyOps = getTxnApplyOpsFilter(opNsMatch["ns"], nss);

    // Either (1) or (3), excluding those resulting from chunk migration.
    BSONObj commandAndApplyOpsMatch =
        BSON("$and" << BSON_ARRAY(BSON(OR(commandMatch, applyOps)) << notFromMigrateFilter));

    // Match oplog entries after "start" that are either supported (1) commands or (2) operations.
    // Only include CRUD operations tagged "fromMigrate" when the "showMigrationEvents" option is
    // set - exempt all other operations and commands with that tag. Include the resume token, if
    // resuming, so we can verify it was still present in the oplog.
    return BSON("$and" << BSON_ARRAY(BSON("ts" << GTE << startFromInclusive)
                                     << BSON(OR(opMatch, commandAndApplyOpsMatch))));
}

namespace {

list<intrusive_ptr<DocumentSource>> buildPipeline(const intrusive_ptr<ExpressionContext>& expCtx,
                                                  const DocumentSourceChangeStreamSpec spec,
                                                  BSONElement elem) {
    list<intrusive_ptr<DocumentSource>> stages;
    boost::optional<Timestamp> startFrom;
    intrusive_ptr<DocumentSource> resumeStage = nullptr;
    boost::optional<ResumeTokenData> startAfterInvalidate;
    bool showMigrationEvents = spec.getShowMigrationEvents();
    uassert(31123,
            "Change streams from mongos may not show migration events.",
            !(expCtx->inMongos && showMigrationEvents));

    auto resumeAfter = spec.getResumeAfter();
    auto startAfter = spec.getStartAfter();
    if (resumeAfter || startAfter) {
        uassert(50865,
                "Do not specify both 'resumeAfter' and 'startAfter' in a $changeStream stage",
                !startAfter || !resumeAfter);

        ResumeToken token = resumeAfter ? resumeAfter.get() : startAfter.get();
        ResumeTokenData tokenData = token.getData();

        // If resuming from an "invalidate" using "startAfter", pass along the resume token data to
        // DocumentSourceCheckInvalidate to signify that another invalidate should not be generated.
        if (startAfter && tokenData.fromInvalidate) {
            startAfterInvalidate = tokenData;
        }

        uassert(ErrorCodes::InvalidResumeToken,
                "Attempting to resume a change stream using 'resumeAfter' is not allowed from an "
                "invalidate notification.",
                !resumeAfter || !tokenData.fromInvalidate);

        // If we are resuming a single-collection stream, the resume token should always contain a
        // UUID unless the token is a high water mark.
        uassert(ErrorCodes::InvalidResumeToken,
                "Attempted to resume a single-collection stream, but the resume token does not "
                "include a UUID.",
                tokenData.uuid || !expCtx->isSingleNamespaceAggregation() ||
                    ResumeToken::isHighWaterMarkToken(tokenData));

        // Store the resume token as the initial postBatchResumeToken for this stream.
        expCtx->initialPostBatchResumeToken = token.toDocument().toBson();

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

    // If we do not have a 'resumeAfter' starting point, check for 'startAtOperationTime'.
    if (auto startAtOperationTime = spec.getStartAtOperationTime()) {
        uassert(40674,
                "Only one type of resume option is allowed, but multiple were found.",
                !resumeStage);
        startFrom = *startAtOperationTime;
        resumeStage = DocumentSourceShardCheckResumability::create(expCtx, *startFrom);
    }

    // We can only run on a replica set, or through mongoS. Confirm that this is the case.
    auto replCoord = repl::ReplicationCoordinator::get(expCtx->opCtx);
    uassert(
        40573,
        "The $changeStream stage is only supported on replica sets",
        expCtx->inMongos ||
            (replCoord &&
             replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet));

    // If we do not have an explicit starting point, we should start from the latest majority
    // committed operation. If we are on mongoS and do not have a starting point, set it to the
    // current clusterTime so that all shards start in sync. We always start one tick beyond the
    // most recent operation, to ensure that the stream does not return it.
    if (!startFrom) {
        const auto currentTime = !expCtx->inMongos
            ? LogicalTime{replCoord->getMyLastAppliedOpTime().getTimestamp()}
            : LogicalClock::get(expCtx->opCtx)->getClusterTime();
        startFrom = currentTime.addTicks(1).asTimestamp();
    }

    // We must always build the DSOplogMatch stage even on mongoS, since our validation logic relies
    // upon the fact that it is always the first stage in the pipeline.
    stages.push_back(DocumentSourceOplogMatch::create(
        DocumentSourceChangeStream::buildMatchFilter(expCtx, *startFrom, showMigrationEvents),
        expCtx));

    // If we haven't already populated the initial PBRT, then we are starting from a specific
    // timestamp rather than a resume token. Initialize the PBRT to a high water mark token.
    if (expCtx->initialPostBatchResumeToken.isEmpty()) {
        expCtx->initialPostBatchResumeToken =
            ResumeToken::makeHighWaterMarkToken(*startFrom).toDocument().toBson();
    }

    // Obtain the current FCV and use it to create the DocumentSourceChangeStreamTransform stage.
    const auto fcv = serverGlobalParams.featureCompatibility.getVersion();
    stages.push_back(
        DocumentSourceChangeStreamTransform::create(expCtx, fcv, elem.embeddedObject()));

    // The resume stage must come after the check invalidate stage so that the former can determine
    // whether the event that matches the resume token should be followed by an "invalidate" event.
    stages.push_back(DocumentSourceCheckInvalidate::create(expCtx, startAfterInvalidate));
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

    // Make sure that it is legal to run this $changeStream before proceeding.
    DocumentSourceChangeStream::assertIsLegalSpecification(expCtx, spec);

    auto fullDocOption = spec.getFullDocument();
    uassert(40575,
            str::stream() << "unrecognized value for the 'fullDocument' option to the "
                             "$changeStream stage. Expected \"default\" or "
                             "\"updateLookup\", got \""
                          << fullDocOption << "\"",
            fullDocOption == "updateLookup"_sd || fullDocOption == "default"_sd);

    const bool shouldLookupPostImage = (fullDocOption == "updateLookup"_sd);

    auto stages = buildPipeline(expCtx, spec, elem);
    if (!expCtx->needsMerge) {
        // There should only be one close cursor stage. If we're on the shards and producing input
        // to be merged, do not add a close cursor stage, since the mongos will already have one.
        stages.push_back(DocumentSourceCloseCursor::create(expCtx));

        // There should be only one post-image lookup stage.  If we're on the shards and producing
        // input to be merged, the lookup is done on the mongos.
        if (shouldLookupPostImage) {
            stages.push_back(DocumentSourceLookupChangePostImage::create(expCtx));
        }
    }
    return stages;
}

BSONObj DocumentSourceChangeStream::replaceResumeTokenInCommand(BSONObj originalCmdObj,
                                                                Document resumeToken) {
    Document originalCmd(originalCmdObj);
    auto pipeline = originalCmd[AggregationRequest::kPipelineName].getArray();
    // A $changeStream must be the first element of the pipeline in order to be able
    // to replace (or add) a resume token.
    invariant(!pipeline[0][DocumentSourceChangeStream::kStageName].missing());

    MutableDocument changeStreamStage(
        pipeline[0][DocumentSourceChangeStream::kStageName].getDocument());
    changeStreamStage[DocumentSourceChangeStreamSpec::kResumeAfterFieldName] = Value(resumeToken);

    // If the command was initially specified with a startAtOperationTime, we need to remove it to
    // use the new resume token.
    changeStreamStage[DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName] = Value();
    pipeline[0] =
        Value(Document{{DocumentSourceChangeStream::kStageName, changeStreamStage.freeze()}});
    MutableDocument newCmd(std::move(originalCmd));
    newCmd[AggregationRequest::kPipelineName] = Value(pipeline);
    return newCmd.freeze().toBson();
}

void DocumentSourceChangeStream::assertIsLegalSpecification(
    const intrusive_ptr<ExpressionContext>& expCtx, const DocumentSourceChangeStreamSpec& spec) {
    // If 'allChangesForCluster' is true, the stream must be opened on the 'admin' database with
    // {aggregate: 1}.
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "A $changeStream with 'allChangesForCluster:true' may only be opened "
                             "on the 'admin' database, and with no collection name; found "
                          << expCtx->ns.ns(),
            !spec.getAllChangesForCluster() ||
                (expCtx->ns.isAdminDB() && expCtx->ns.isCollectionlessAggregateNS()));

    // Prevent $changeStream from running on internal databases. A stream may run against the
    // 'admin' database iff 'allChangesForCluster' is true. A stream may run against the 'config'
    // database iff 'allowToRunOnConfigDB' is true.
    const bool isNotBannedInternalDB =
        !expCtx->ns.isLocal() && (!expCtx->ns.isConfigDB() || spec.getAllowToRunOnConfigDB());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "$changeStream may not be opened on the internal " << expCtx->ns.db()
                          << " database",
            expCtx->ns.isAdminDB() ? spec.getAllChangesForCluster() : isNotBannedInternalDB);

    // Prevent $changeStream from running on internal collections in any database.
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "$changeStream may not be opened on the internal " << expCtx->ns.ns()
                          << " collection",
            !expCtx->ns.isSystem());
}

}  // namespace mongo
