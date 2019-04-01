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

#include <memory>
#include <utility>

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

/**
 * GroupFromFirstTransformation consists of a list of (field name, expression pairs). It returns a
 * document synthesized by assigning each field name in the output document to the result of
 * evaluating the corresponding expression. If the expression evaluates to missing, we assign a
 * value of BSONNULL. This is necessary to match the semantics of $first for missing fields.
 */
class GroupFromFirstDocumentTransformation final : public TransformerInterface {
public:
    GroupFromFirstDocumentTransformation(
        const std::string& groupId,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> accumulatorExprs)
        : _accumulatorExprs(std::move(accumulatorExprs)), _groupId(groupId) {}

    TransformerType getType() const final {
        return TransformerType::kGroupFromFirstDocument;
    }

    /**
     * The path of the field that we are grouping on: i.e., the field in the input document that we
     * will use to create the _id field of the ouptut document.
     */
    const std::string& groupId() const {
        return _groupId;
    }

    Document applyTransformation(const Document& input) final;

    void optimize() final;

    Document serializeTransformation(
        boost::optional<ExplainOptions::Verbosity> explain) const final;

    DepsTracker::State addDependencies(DepsTracker* deps) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    static std::unique_ptr<GroupFromFirstDocumentTransformation> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::string& groupId,
        std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> accumulatorExprs);

private:
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> _accumulatorExprs;
    std::string _groupId;
};

class DocumentSourceGroup final : public DocumentSource {
public:
    using Accumulators = std::vector<boost::intrusive_ptr<Accumulator>>;
    using GroupsMap = ValueUnorderedMap<Accumulators>;

    static constexpr StringData kStageName = "$group"_sd;

    boost::intrusive_ptr<DocumentSource> optimize() final;
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    GetModPathsReturn getModifiedPaths() const final;

    /**
     * Convenience method for creating a new $group stage. If maxMemoryUsageBytes is boost::none,
     * then it will actually use the value of internalDocumentSourceGroupMaxMemoryBytes.
     */
    static boost::intrusive_ptr<DocumentSourceGroup> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        std::vector<AccumulationStatement> accumulationStatements,
        boost::optional<size_t> maxMemoryUsageBytes = boost::none);

    /**
     * Parses 'elem' into a $group stage, or throws a AssertionException if 'elem' was an invalid
     * specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kBlocking,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kWritesTmpData,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed};
    }

    /**
     * Add an accumulator, which will become a field in each Document that results from grouping.
     */
    void addAccumulator(AccumulationStatement accumulationStatement);

    /**
     * Sets the expression to use to determine the group id of each document.
     */
    void setIdExpression(const boost::intrusive_ptr<Expression> idExpression);

    /**
     * Returns true if this $group stage represents a 'global' $group which is merging together
     * results from earlier partial groups.
     */
    bool doingMerge() const {
        return _doingMerge;
    }

    /**
     * Tell this source if it is doing a merge from shards. Defaults to false.
     */
    void setDoingMerge(bool doingMerge) {
        _doingMerge = doingMerge;
    }

    /**
     * Returns true if this $group stage used disk during execution and false otherwise.
     */
    bool usedDisk() final;

    boost::optional<MergingLogic> mergingLogic() final;
    bool canRunInParallelBeforeOut(
        const std::set<std::string>& nameOfShardKeyFieldsUponEntryToStage) const final;

    /**
     * When possible, creates a document transformer that transforms the first document in a group
     * into one of the output documents of the $group stage. This is possible when we are grouping
     * on a single field and all accumulators are $first (or there are no accumluators).
     *
     * It is sometimes possible to use a DISTINCT_SCAN to scan the first document of each group,
     * in which case this transformation can replace the actual $group stage in the pipeline
     * (SERVER-9507).
     */
    std::unique_ptr<GroupFromFirstDocumentTransformation> rewriteGroupAsTransformOnFirstDocument()
        const;

    void acceptVisitor(DocumentSourceVisitor* visitor) final {
        visitor->visit(this);
    }

protected:
    void doDispose() final;

private:
    explicit DocumentSourceGroup(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                 boost::optional<size_t> maxMemoryUsageBytes = boost::none);

    ~DocumentSourceGroup();

    /**
     * getNext() dispatches to one of these three depending on what type of $group it is. These
     * methods expect '_currentAccumulators' to have been reset before being called, and also expect
     * initialize() to have been called already.
     */
    GetNextResult getNextSpilled();
    GetNextResult getNextStandard();

    /**
     * Before returning anything, this source must prepare itself. In a streaming $group,
     * initialize() requests the first document from the previous source, and uses it to prepare the
     * accumulators. In an unsorted $group, initialize() exhausts the previous source before
     * returning. The '_initialized' boolean indicates that initialize() has finished.
     *
     * This method may not be able to finish initialization in a single call if 'pSource' returns a
     * DocumentSource::GetNextResult::kPauseExecution, so it returns the last GetNextResult
     * encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult initialize();

    /**
     * Spill groups map to disk and returns an iterator to the file. Note: Since a sorted $group
     * does not exhaust the previous stage before returning, and thus does not maintain as large a
     * store of documents at any one time, only an unsorted group can spill to disk.
     */
    std::shared_ptr<Sorter<Value, Value>::Iterator> spill();

    Document makeDocument(const Value& id, const Accumulators& accums, bool mergeableOutput);

    /**
     * Computes the internal representation of the group key.
     */
    Value computeId(const Document& root);

    /**
     * Converts the internal representation of the group key to the _id shape specified by the
     * user.
     */
    Value expandId(const Value& val);

    /**
     * Returns true if 'dottedPath' is one of the group keys present in '_idExpressions'.
     */
    bool pathIncludedInGroupKeys(const std::string& dottedPath) const;

    std::vector<AccumulationStatement> _accumulatedFields;

    bool _usedDisk;  // Keeps track of whether this $group spilled to disk.
    bool _doingMerge;
    size_t _memoryUsageBytes = 0;
    size_t _maxMemoryUsageBytes;
    std::string _fileName;
    unsigned int _nextSortedFileWriterOffset = 0;
    bool _ownsFileDeletion = true;  // unless a MergeIterator is made that takes over.

    std::vector<std::string> _idFieldNames;  // used when id is a document
    std::vector<boost::intrusive_ptr<Expression>> _idExpressions;

    bool _initialized;

    Value _currentId;
    Accumulators _currentAccumulators;

    // We use boost::optional to defer initialization until the ExpressionContext containing the
    // correct comparator is injected, since the groups must be built using the comparator's
    // definition of equality.
    boost::optional<GroupsMap> _groups;

    std::vector<std::shared_ptr<Sorter<Value, Value>::Iterator>> _sortedFiles;
    bool _spilled;

    // Only used when '_spilled' is false.
    GroupsMap::iterator groupsIterator;

    // Only used when '_spilled' is true.
    std::unique_ptr<Sorter<Value, Value>::Iterator> _sorterIterator;
    const bool _allowDiskUse;

    std::pair<Value, Value> _firstPartOfNextGroup;
};

}  // namespace mongo
