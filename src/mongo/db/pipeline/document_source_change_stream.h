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

#pragma once

#include <type_traits>

#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

/**
 * The $changeStream stage is an alias for a cursor on oplog followed by a $match stage and a
 * transform stage on mongod.
 */
class DocumentSourceChangeStream final {
public:
    class LiteParsed : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec) {
            return std::make_unique<LiteParsed>(spec.fieldName(), nss);
        }

        explicit LiteParsed(std::string parseTimeName, NamespaceString nss)
            : LiteParsedDocumentSource(std::move(parseTimeName)), _nss(std::move(nss)) {}

        bool isChangeStream() const final {
            return true;
        }

        bool allowedToPassthroughFromMongos() const final {
            return false;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        ActionSet actions{ActionType::changeStream, ActionType::find};
        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
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

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const {
            // Change streams require "majority" readConcern. If the client did not specify an
            // explicit readConcern, change streams will internally upconvert the readConcern to
            // majority (so clients can always send aggregations without readConcern). We therefore
            // do not permit the cluster-wide default to be applied.
            return onlySingleReadConcernSupported(
                kStageName, repl::ReadConcernLevel::kMajorityReadConcern, level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const {
            transactionNotSupported(kStageName);
        }

    private:
        const NamespaceString _nss;
    };

    // The name of the field where the document key (_id and shard key, if present) will be found
    // after the transformation.
    static constexpr StringData kDocumentKeyField = "documentKey"_sd;

    // The name of the field where the pre-image document will be found, if requested and available.
    static constexpr StringData kFullDocumentBeforeChangeField = "fullDocumentBeforeChange"_sd;

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

    // This UUID field represents all of:
    // 1. The UUID for a particular resharding operation.
    // 2. The UUID for the temporary collection that exists during a resharding operation.
    // 3. The UUID for a collection being resharded, once a resharding operation has completed.
    static constexpr StringData kReshardingUuidField = "reshardingUUID"_sd;

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
    static constexpr StringData kTxnOpIndexField = "txnOpIndex"_sd;

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
    static constexpr StringData kReshardBeginOpType = "reshardBegin"_sd;
    static constexpr StringData kReshardDoneCatchUpOpType = "reshardDoneCatchUp"_sd;
    // Internal op type to signal mongos to open cursors on new shards.
    static constexpr StringData kNewShardDetectedOpType = "kNewShardDetected"_sd;

    static constexpr StringData kRegexAllCollections = R"((?!(\$|system\.)))"_sd;
    static constexpr StringData kRegexAllDBs = R"(^(?!(admin|config|local)\.)[^.]+)"_sd;
    static constexpr StringData kRegexCmdColl = R"(\$cmd$)"_sd;

    enum class ChangeStreamType { kSingleCollection, kSingleDatabase, kAllChangesForCluster };

    /**
     * Helpers for Determining which regex to match a change stream against.
     */
    static ChangeStreamType getChangeStreamType(const NamespaceString& nss);
    static std::string getNsRegexForChangeStream(const NamespaceString& nss);

    /**
     * Parses a $changeStream stage from 'elem' and produces the $match and transformation
     * stages required.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Helper used by various change stream stages. Used for asserting that a certain Value of a
     * field has a certain type. Will uassert() if the field does not have the expected type.
     */
    static void checkValueType(const Value v, const StringData fieldName, BSONType expectedType);

private:
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
 * A LiteParse class to be used to register all internal change stream stages. This class will
 * ensure that all the necessary authentication and input validation checks are applied while
 * parsing.
 */
class LiteParsedDocumentSourceChangeStreamInternal final
    : public DocumentSourceChangeStream::LiteParsed {
public:
    static std::unique_ptr<LiteParsedDocumentSourceChangeStreamInternal> parse(
        const NamespaceString& nss, const BSONElement& spec) {
        return std::make_unique<LiteParsedDocumentSourceChangeStreamInternal>(spec.fieldName(),
                                                                              nss);
    }

    LiteParsedDocumentSourceChangeStreamInternal(std::string parseTimeName, NamespaceString nss)
        : DocumentSourceChangeStream::LiteParsed(std::move(parseTimeName), std::move(nss)) {}

    PrivilegeVector requiredPrivileges(bool isMongos,
                                       bool bypassDocumentValidation) const override final {
        return {Privilege(ResourcePattern::forClusterResource(), ActionType::internal)};
    }
};

/**
 * Class interface to keep track of the change streams internal stage serialization formats across
 * versions or features.
 *
 * TODO SERVER-55659: remove this serializer class and make each stage serialize only the "latest"
 * format.
 */
class ChangeStreamStageSerializationInterface {
public:
    Value serializeToValue(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const {
        return feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV()
            ? serializeLatest(explain)
            : serializeLegacy(explain);
    }

protected:
    virtual Value serializeLegacy(boost::optional<ExplainOptions::Verbosity> explain) const = 0;
    virtual Value serializeLatest(boost::optional<ExplainOptions::Verbosity> explain) const = 0;
};

}  // namespace mongo
