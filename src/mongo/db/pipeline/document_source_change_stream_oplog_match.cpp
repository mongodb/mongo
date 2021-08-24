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

#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/bson/bson_helper.h"

namespace mongo {


REGISTER_INTERNAL_DOCUMENT_SOURCE(
    _internalChangeStreamOplogMatch,
    LiteParsedDocumentSourceChangeStreamInternal::parse,
    DocumentSourceOplogMatch::createFromBson,
    feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV());

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

    // "o.applyOps" stores the list of operations, so it must be an array.
    applyOpsBuilder.append("o.applyOps",
                           BSON("$type"
                                << "array"));
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

/**
 * Produce the BSON object representing the filter for the $match stage to filter oplog entries
 * to only those relevant for this $changeStream stage.
 */
BSONObj buildMatchFilter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         Timestamp startFromInclusive,
                         bool showMigrationEvents) {
    auto nss = expCtx->ns;

    DocumentSourceChangeStream::ChangeStreamType sourceType =
        DocumentSourceChangeStream::getChangeStreamType(nss);

    // 1) Supported commands that have the target db namespace (e.g. test.$cmd) in "ns" field.
    BSONArrayBuilder relevantCommands;

    if (sourceType == DocumentSourceChangeStream::ChangeStreamType::kSingleCollection) {
        relevantCommands.append(BSON("o.drop" << nss.coll()));
        // Generate 'rename' entries if the change stream is open on the source or target namespace.
        relevantCommands.append(BSON("o.renameCollection" << nss.ns()));
        relevantCommands.append(BSON("o.to" << nss.ns()));
    } else {
        // For change streams on an entire database, include notifications for drops and renames of
        // non-system collections which will not invalidate the stream. Also include the
        // 'dropDatabase' command which will invalidate the stream.
        relevantCommands.append(
            BSON("o.drop" << BSONRegEx("^" + DocumentSourceChangeStream::kRegexAllCollections)));
        relevantCommands.append(BSON("o.dropDatabase" << BSON("$exists" << true)));
        relevantCommands.append(
            BSON("o.renameCollection"
                 << BSONRegEx(DocumentSourceChangeStream::getNsRegexForChangeStream(nss))));
    }

    // For cluster-wide $changeStream, match the command namespace of any database other than admin,
    // config, or local. Otherwise, match only against the target db's command namespace.
    auto cmdNsFilter =
        (sourceType == DocumentSourceChangeStream::ChangeStreamType::kAllChangesForCluster
             ? BSON("ns" << BSONRegEx(DocumentSourceChangeStream::kRegexAllDBs + "\\." +
                                      DocumentSourceChangeStream::kRegexCmdColl))
             : BSON("ns" << nss.getCommandNS().ns()));

    // 1.1) Commands that are on target db(s) and one of the above supported commands.
    auto commandsOnTargetDb =
        BSON("$and" << BSON_ARRAY(cmdNsFilter << BSON("$or" << relevantCommands.arr())));

    // 1.2) Supported commands that have arbitrary db namespaces in "ns" field.
    auto renameDropTarget =
        BSON("o.to" << BSONRegEx(DocumentSourceChangeStream::getNsRegexForChangeStream(nss)));

    // 1.3) Transaction commit commands.
    auto transactionCommit = BSON("o.commitTransaction" << 1);

    // All supported commands that are either (1.1), (1.2) or (1.3).
    BSONObj commandMatch = BSON("op"
                                << "c"
                                << OR(commandsOnTargetDb, renameDropTarget, transactionCommit));

    // 2) Supported operations on the operation namespace, optionally including those from
    // migrations.
    BSONObj opNsMatch =
        BSON("ns" << BSONRegEx(DocumentSourceChangeStream::getNsRegexForChangeStream(nss)));

    // 2.1) Normal CRUD ops.
    auto normalOpTypeMatch = BSON("op" << NE << "n");

    // TODO SERVER-44039: we continue to generate 'kNewShardDetected' events for compatibility
    // with 4.2, even though we no longer rely on them to detect new shards. We may wish to remove
    // this mechanism in 4.7+, or retain it for future cases where a change stream is targeted to a
    // subset of shards. See SERVER-44039 for details.

    // 2.2) Noop change events:
    //     a) migrateChunkToNewShard: A chunk gets migrated to a new shard that doesn't have any
    //     chunks.
    //     b) reshardBegin: A resharding operation begins.
    //     c) reshardDoneCatchUp: Data is now strictly consistent on the temporary resharding
    //        collection.
    static const std::vector<StringData> internalOpTypes = {
        "migrateChunkToNewShard", "reshardBegin", "reshardDoneCatchUp"};
    BSONArrayBuilder internalOpTypeOrBuilder;
    for (const auto& eventName : internalOpTypes) {
        internalOpTypeOrBuilder.append(BSON("o2.type" << eventName));
    }
    internalOpTypeOrBuilder.done();

    auto internalOpTypeMatch = BSON("op"
                                    << "n"
                                    << "$or" << internalOpTypeOrBuilder.arr());

    // Supported operations that are either (2.1) or (2.2).
    BSONObj normalOrInternalOpTypeMatch =
        BSON(opNsMatch["ns"] << OR(normalOpTypeMatch, internalOpTypeMatch));

    // Filter excluding entries resulting from chunk migration.
    BSONObj notFromMigrateFilter = BSON("fromMigrate" << NE << true);

    BSONObj opMatch =
        (showMigrationEvents
             ? normalOrInternalOpTypeMatch
             : BSON("$and" << BSON_ARRAY(normalOrInternalOpTypeMatch << notFromMigrateFilter)));

    // 3) Look for 'applyOps' which were created as part of a transaction.
    BSONObj applyOps = getTxnApplyOpsFilter(opNsMatch["ns"], nss);

    // Exclude 'fromMigrate' events if 'showMigrationEvents' has not been specified as $changeStream
    // option.
    BSONObj commandAndApplyOpsMatch =
        (showMigrationEvents ? BSON(OR(commandMatch, applyOps))
                             : BSON("$and" << BSON_ARRAY(BSON(OR(commandMatch, applyOps))
                                                         << notFromMigrateFilter)));

    // Match oplog entries after "start" that are either supported (1) commands or (2) operations.
    // Only include CRUD operations tagged "fromMigrate" when the "showMigrationEvents" option is
    // set - exempt all other operations and commands with that tag. Include the resume token, if
    // resuming, so we can verify it was still present in the oplog.
    return BSON("$and" << BSON_ARRAY(BSON("ts" << GTE << startFromInclusive)
                                     << BSON(OR(opMatch, commandAndApplyOpsMatch))));
}

}  // namespace

boost::intrusive_ptr<DocumentSourceOplogMatch> DocumentSourceOplogMatch::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool showMigrationEvents) {
    // TODO SERVER-56669: ensure that 'initialPostBatchResumeToken' is always populated at this
    // point.
    return make_intrusive<DocumentSourceOplogMatch>(
        buildMatchFilter(
            expCtx,
            ResumeToken::parse(expCtx->initialPostBatchResumeToken).getData().clusterTime,
            showMigrationEvents),
        expCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOplogMatch::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(5467600,
            "the match filter must be an expression in an object",
            elem.type() == BSONType::Object);
    auto parsedSpec = DocumentSourceChangeStreamOplogMatchSpec::parse(
        IDLParserErrorContext("DocumentSourceChangeStreamOplogMatchSpec"), elem.Obj());
    return make_intrusive<DocumentSourceOplogMatch>(parsedSpec.getFilter(), pExpCtx);
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
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);
    constraints.isIndependentOfAnyCollection =
        pExpCtx->ns.isCollectionlessAggregateNS() ? true : false;
    return constraints;
}

Value DocumentSourceOplogMatch::serializeLatest(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        return Value(
            Document{{DocumentSourceChangeStream::kStageName,
                      Document{{"stage"_sd, "internalOplogMatch"_sd}, {"filter"_sd, _predicate}}}});
    }

    DocumentSourceChangeStreamOplogMatchSpec spec(_predicate);
    return Value(Document{{DocumentSourceOplogMatch::kStageName, spec.toBSON()}});
}

}  // namespace mongo
