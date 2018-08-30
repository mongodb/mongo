/**
 * Copyright (C) 2016 MongoDB Inc.
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
#include "mongo/db/pipeline/document_source_out_gen.h"

namespace mongo {

/**
 * Abstract class for the $out aggregation stage.
 */
class DocumentSourceOut : public DocumentSource, public NeedsMergerDocumentSource {
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

    private:
        bool _allowShardedOutNss;
    };

    DocumentSourceOut(NamespaceString outputNs,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      WriteModeEnum mode,
                      std::set<FieldPath> uniqueKey);

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
        return {StreamType::kStreaming,
                PositionRequirement::kLast,
                // A $out to an unsharded collection should merge on the primary shard to perform
                // local writes. A $out to a sharded collection has no requirement, since each shard
                // can perform its own portion of the write.
                HostTypeRequirement::kPrimaryShard,
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

    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return nullptr;
    }
    MergingLogic mergingLogic() final {
        return {this};
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
        void emplace(BSONObj obj, BSONObj key) {
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
    virtual void spill(const BatchedObjects& batch) {
        pExpCtx->mongoProcessInterface->insert(pExpCtx, getWriteNs(), batch.objects);
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
        std::set<FieldPath> uniqueKey = std::set<FieldPath>{"_id"});

    /**
     * Parses a $out stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    bool _initialized = false;
    bool _done = false;

    const NamespaceString _outputNs;
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
