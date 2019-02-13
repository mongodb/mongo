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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_out_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

/**
 * Manipulates the state of the OperationContext so that while this object is in scope, reads and
 * writes will use a local read concern and see the latest version of the data. Resets the
 * OperationContext back to its original state upon destruction.
 */
class LocalReadConcernBlock {
    OperationContext* _opCtx;
    repl::ReadConcernArgs _originalArgs;
    RecoveryUnit::ReadSource _originalSource;

public:
    LocalReadConcernBlock(OperationContext* opCtx) : _opCtx(opCtx) {
        _originalArgs = repl::ReadConcernArgs::get(_opCtx);
        _originalSource = _opCtx->recoveryUnit()->getTimestampReadSource();

        repl::ReadConcernArgs::get(_opCtx) = repl::ReadConcernArgs();
        _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::kUnset);
    }

    ~LocalReadConcernBlock() {
        repl::ReadConcernArgs::get(_opCtx) = _originalArgs;
        _opCtx->recoveryUnit()->setTimestampReadSource(_originalSource);
    }
};

/**
 * Abstract class for the $out aggregation stage.
 */
class DocumentSourceOut : public DocumentSource {
public:
    /**
     * A "lite parsed" $out stage is similar to other stages involving foreign collections except in
     * some cases the foreign collection is allowed to be sharded.
     */
    class LiteParsed final : public LiteParsedDocumentSourceForeignCollections {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec);

        LiteParsed(NamespaceString outNss, PrivilegeVector privileges, bool allowShardedOutNss)
            : LiteParsedDocumentSourceForeignCollections(outNss, privileges),
              _allowShardedOutNss(allowShardedOutNss) {}

        bool allowShardedForeignCollection(NamespaceString nss) const final {
            return _allowShardedOutNss ? true : (_foreignNssSet.find(nss) == _foreignNssSet.end());
        }

        bool allowedToPassthroughFromMongos() const final {
            // Do not allow passthrough from mongos even if the source collection is unsharded. This
            // ensures that the unique index verification happens once on mongos and can be bypassed
            // on the shards.
            return false;
        }

    private:
        bool _allowShardedOutNss;
    };

    /**
     * Builds a new $out stage which will spill all documents into 'outputNs' as inserts. If
     * 'targetCollectionVersion' is provided then processing will stop with an error if the
     * collection's epoch changes during the course of execution. This is used as a mechanism to
     * prevent the shard key from changing.
     */
    DocumentSourceOut(NamespaceString outputNs,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      WriteModeEnum mode,
                      std::set<FieldPath> uniqueKey,
                      boost::optional<ChunkVersion> targetCollectionVersion);

    virtual ~DocumentSourceOut() = default;

    GetNextResult getNext() final;
    const char* getSourceName() const final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    /**
     * For purposes of tracking which fields come from where, this stage does not modify any fields.
     */
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, std::set<std::string>{}, {}};
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        // A $out to an unsharded collection should merge on the primary shard to perform local
        // writes. A $out to a sharded collection has no requirement, since each shard can perform
        // its own portion of the write. We use 'kAnyShard' to direct it to execute on one of the
        // shards in case some of the writes happen to end up being local.
        //
        // Note that this decision is inherently racy and subject to become stale. This is okay
        // because either choice will work correctly, we are simply applying a heuristic
        // optimization.
        auto hostTypeRequirement = HostTypeRequirement::kPrimaryShard;
        if (pExpCtx->mongoProcessInterface->isSharded(pExpCtx->opCtx, _outputNs) &&
            _mode != WriteModeEnum::kModeReplaceCollection) {
            hostTypeRequirement = HostTypeRequirement::kAnyShard;
        }
        return {StreamType::kStreaming,
                PositionRequirement::kLast,
                hostTypeRequirement,
                DiskUseRequirement::kWritesPersistentData,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kNotAllowed};
    }

    const NamespaceString& getOutputNs() const {
        return _outputNs;
    }

    WriteModeEnum getMode() const {
        return _mode;
    }

    boost::optional<MergingLogic> mergingLogic() final {
        // It should always be faster to avoid splitting the pipeline if the output collection is
        // sharded. If we avoid splitting the pipeline then each shard can perform the writes to the
        // target collection in parallel.
        //
        // Note that this decision is inherently racy and subject to become stale. This is okay
        // because either choice will work correctly, we are simply applying a heuristic
        // optimization.
        if (pExpCtx->mongoProcessInterface->isSharded(pExpCtx->opCtx, _outputNs)) {
            return boost::none;
        }
        // {shardsStage, mergingStage, sortPattern}
        return MergingLogic{nullptr, this, boost::none};
    }

    virtual bool canRunInParallelBeforeOut(
        const std::set<std::string>& nameOfShardKeyFieldsUponEntryToStage) const final {
        // If someone is asking the question, this must be the $out stage in question, so yes!
        return true;
    }


    /**
     * Retrieves the namespace to direct each batch to, which may be a temporary namespace or the
     * final output namespace.
     */
    virtual const NamespaceString& getWriteNs() const = 0;

    /**
     * Prepares the DocumentSource to be able to write incoming batches to the desired collection.
     */
    virtual void initializeWriteNs() = 0;

    /**
     * Storage for a batch of BSON Objects to be inserted/updated to the write namespace. The
     * extracted unique key values are also stored in a batch, used by $out with mode
     * "replaceDocuments" as the query portion of the update.
     *
     */
    struct BatchedObjects {
        void emplace(BSONObj&& obj, BSONObj&& key) {
            objects.emplace_back(std::move(obj));
            uniqueKeys.emplace_back(std::move(key));
        }

        bool empty() const {
            return objects.empty();
        }

        size_t size() const {
            return objects.size();
        }

        void clear() {
            objects.clear();
            uniqueKeys.clear();
        }

        std::vector<BSONObj> objects;
        // Store the unique keys as BSON objects instead of Documents for compatibility with the
        // batch update command. (e.g. {q: <array of uniqueKeys>, u: <array of objects>})
        std::vector<BSONObj> uniqueKeys;
    };

    /**
     * Writes the documents in 'batch' to the write namespace.
     */
    virtual void spill(BatchedObjects&& batch) {
        LocalReadConcernBlock readLocal(pExpCtx->opCtx);

        pExpCtx->mongoProcessInterface->insert(
            pExpCtx, getWriteNs(), std::move(batch.objects), _writeConcern, _targetEpoch());
    };

    /**
     * Finalize the output collection, called when there are no more documents to write.
     */
    virtual void finalize() = 0;

    /**
     * Creates a new $out stage from the given arguments.
     */
    static boost::intrusive_ptr<DocumentSourceOut> create(
        NamespaceString outputNs,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        WriteModeEnum,
        std::set<FieldPath> uniqueKey = std::set<FieldPath>{"_id"},
        boost::optional<ChunkVersion> targetCollectionVersion = boost::none);

    /**
     * Parses a $out stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

protected:
    // Stash the writeConcern of the original command as the operation context may change by the
    // time we start to spill $out writes. This is because certain aggregations (e.g. $exchange)
    // establish cursors with batchSize 0 then run subsequent getMore's which use a new operation
    // context. The getMore's will not have an attached writeConcern however we still want to
    // respect the writeConcern of the original command.
    WriteConcernOptions _writeConcern;

    const NamespaceString _outputNs;
    boost::optional<ChunkVersion> _targetCollectionVersion;

    boost::optional<OID> _targetEpoch() {
        return _targetCollectionVersion ? boost::optional<OID>(_targetCollectionVersion->epoch())
                                        : boost::none;
    }

private:
    /**
     * If 'spec' does not specify a uniqueKey, uses the sharding catalog to pick a default key of
     * the shard key + _id. Returns a pair of the uniqueKey (either from the spec or generated), and
     * an optional ChunkVersion, populated with the version stored in the sharding catalog when we
     * asked for the shard key.
     */
    static std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>> resolveUniqueKeyOnMongoS(
        const boost::intrusive_ptr<ExpressionContext>&,
        const DocumentSourceOutSpec& spec,
        const NamespaceString& outputNs);

    /**
     * Ensures that 'spec' contains a uniqueKey which has a supporting index - either because the
     * uniqueKey was sent from mongos or because there is a corresponding unique index. Returns the
     * target ChunkVersion already attached to 'spec', but verifies that this node's cached routing
     * table agrees on the epoch for that version before returning. Throws a StaleConfigException if
     * not.
     */
    static std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>> resolveUniqueKeyOnMongoD(
        const boost::intrusive_ptr<ExpressionContext>&,
        const DocumentSourceOutSpec& spec,
        const NamespaceString& outputNs);

    bool _initialized = false;
    bool _done = false;

    WriteModeEnum _mode;

    // Holds the unique key used for uniquely identifying documents. There must exist a unique index
    // with this key pattern (up to order). Default is "_id" for unsharded collections, and "_id"
    // plus the shard key for sharded collections.
    std::set<FieldPath> _uniqueKeyFields;

    // True if '_uniqueKeyFields' contains the _id. We store this as a separate boolean to avoid
    // repeated lookups into the set.
    bool _uniqueKeyIncludesId;
};

}  // namespace mongo
