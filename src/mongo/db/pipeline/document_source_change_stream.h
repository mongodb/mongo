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

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_sources_gen.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/resume_token.h"

namespace mongo {

/**
 * The $changeStream stage is an alias for a cursor on oplog followed by a $match stage and a
 * transform stage on mongod.
 */
class DocumentSourceChangeStream final {
public:
    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec) {
            return stdx::make_unique<LiteParsed>(request.getNamespaceString());
        }

        explicit LiteParsed(NamespaceString nss) : _nss(std::move(nss)) {}

        bool isChangeStream() const final {
            return true;
        }

        bool allowedToForwardFromMongos() const final {
            return false;
        }

        bool allowedToPassthroughFromMongos() const final {
            return false;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        ActionSet actions{ActionType::changeStream, ActionType::find};
        PrivilegeVector requiredPrivileges(bool isMongos) const final {
            if (_nss.isAdminDB() && _nss.isCollectionlessAggregateNS()) {
                // Watching a whole cluster.
                return {Privilege(ResourcePattern::forAnyNormalResource(), actions)};
            } else if (_nss.isCollectionlessAggregateNS()) {
                // Watching a whole database.
                return {Privilege(ResourcePattern::forDatabaseName(_nss.db()), actions)};
            } else {
                // Watching a single collection. Note if this is in the admin database it will fail
                // at parse time.
                return {Privilege(ResourcePattern::forExactNamespace(_nss), actions)};
            }
        }

        void assertSupportsReadConcern(const repl::ReadConcernArgs& readConcern) const {
            // Only "majority" is allowed for change streams.
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Read concern " << readConcern.toString()
                                  << " is not supported for change streams. "
                                     "Only read concern level \"majority\" is supported.",
                    !readConcern.hasLevel() ||
                        readConcern.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern);
        }

    private:
        const NamespaceString _nss;
    };

    // The name of the field where the document key (_id and shard key, if present) will be found
    // after the transformation.
    static constexpr StringData kDocumentKeyField = "documentKey"_sd;

    // The name of the field where the full document will be found after the transformation. The
    // full document is only present for certain types of operations, such as an insert.
    static constexpr StringData kFullDocumentField = "fullDocument"_sd;

    // The name of the field where the change identifier will be located after the transformation.
    static constexpr StringData kIdField = "_id"_sd;

    // The name of the field where the namespace of the change will be located after the
    // transformation.
    static constexpr StringData kNamespaceField = "ns"_sd;

    // Name of the field which stores information about updates. Only applies when OperationType
    // is "update".
    static constexpr StringData kUpdateDescriptionField = "updateDescription"_sd;

    // The name of the subfield of '_id' where the UUID of the namespace will be located after the
    // transformation.
    static constexpr StringData kUuidField = "uuid"_sd;

    // The name of the field where the type of the operation will be located after the
    // transformation.
    static constexpr StringData kOperationTypeField = "operationType"_sd;

    // The name of the field where the clusterTime of the change will be located after the
    // transformation. The cluster time will be located inside the change identifier, so the full
    // path to the cluster time will be kIdField + "." + kClusterTimeField.
    static constexpr StringData kClusterTimeField = "clusterTime"_sd;

    // The name of this stage.
    static constexpr StringData kStageName = "$changeStream"_sd;

    static constexpr StringData kTxnNumberField = "txnNumber"_sd;
    static constexpr StringData kLsidField = "lsid"_sd;

    // The target namespace of a rename operation.
    static constexpr StringData kRenameTargetNssField = "to"_sd;

    // The different types of operations we can use for the operation type.
    static constexpr StringData kUpdateOpType = "update"_sd;
    static constexpr StringData kDeleteOpType = "delete"_sd;
    static constexpr StringData kReplaceOpType = "replace"_sd;
    static constexpr StringData kInsertOpType = "insert"_sd;
    static constexpr StringData kDropCollectionOpType = "drop"_sd;
    static constexpr StringData kRenameCollectionOpType = "rename"_sd;
    static constexpr StringData kDropDatabaseOpType = "dropDatabase"_sd;
    static constexpr StringData kInvalidateOpType = "invalidate"_sd;
    // Internal op type to signal mongos to open cursors on new shards.
    static constexpr StringData kNewShardDetectedOpType = "kNewShardDetected"_sd;

    enum class ChangeStreamType { kSingleCollection, kSingleDatabase, kAllChangesForCluster };


    /**
     * Helpers for Determining which regex to match a change stream against.
     */
    static ChangeStreamType getChangeStreamType(const NamespaceString& nss);
    static std::string getNsRegexForChangeStream(const NamespaceString& nss);


    /**
     * Produce the BSON object representing the filter for the $match stage to filter oplog entries
     * to only those relevant for this $changeStream stage.
     */
    static BSONObj buildMatchFilter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    Timestamp startFrom,
                                    bool startFromInclusive);

    /**
     * Parses a $changeStream stage from 'elem' and produces the $match and transformation
     * stages required.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Given a BSON object containing an aggregation command with a $changeStream stage, and a
     * resume token, returns a new BSON object with the same command except with the addition of a
     * resumeAfter: option containing the resume token.  If there was a previous resumeAfter:
     * option, it is removed.
     */
    static BSONObj replaceResumeTokenInCommand(const BSONObj originalCmdObj,
                                               const BSONObj resumeToken);

    /**
     * Helper used by various change stream stages. Used for asserting that a certain Value of a
     * field has a certain type. Will uassert() if the field does not have the expected type.
     */
    static void checkValueType(const Value v, const StringData fieldName, BSONType expectedType);

private:
    static constexpr StringData kRegexAllCollections = R"((?!(\$|system\.)))"_sd;
    static constexpr StringData kRegexAllDBs = R"(^(?!(admin|config|local)\.)[^.]+)"_sd;
    static constexpr StringData kRegexCmdColl = R"(\$cmd$)"_sd;

    // Helper function which throws if the $changeStream fails any of a series of semantic checks.
    // For instance, whether it is permitted to run given the current FCV, whether the namespace is
    // valid for the options specified in the spec, etc.
    static void assertIsLegalSpecification(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const DocumentSourceChangeStreamSpec& spec);

    // It is illegal to construct a DocumentSourceChangeStream directly, use createFromBson()
    // instead.
    DocumentSourceChangeStream() = default;
};

/**
 * A custom subclass of DocumentSourceMatch which does not serialize itself (since it came from an
 * alias) and requires itself to be the first stage in the pipeline.
 */
class DocumentSourceOplogMatch final : public DocumentSourceMatch {
public:
    static boost::intrusive_ptr<DocumentSourceOplogMatch> create(
        BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    const char* getSourceName() const final;

    GetNextResult getNext() final {
        // We should never execute this stage directly. We expect this stage to be absorbed into the
        // cursor feeding the pipeline, and executing this stage may result in the use of the wrong
        // collation. The comparisons against the oplog must use the simple collation, regardless of
        // the collation on the ExpressionContext.
        MONGO_UNREACHABLE;
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const final;

private:
    DocumentSourceOplogMatch(BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx);
};

}  // namespace mongo
