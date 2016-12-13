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

#include <memory>
#include <utility>

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

class DocumentSourceGroup final : public DocumentSource, public SplittableDocumentSource {
public:
    using Accumulators = std::vector<boost::intrusive_ptr<Accumulator>>;
    using GroupsMap = ValueUnorderedMap<Accumulators>;

    static const size_t kDefaultMaxMemoryUsageBytes = 100 * 1024 * 1024;

    // Virtuals from DocumentSource.
    boost::intrusive_ptr<DocumentSource> optimize() final;
    GetDepsReturn getDependencies(DepsTracker* deps) const final;
    Value serialize(bool explain = false) const final;
    GetNextResult getNext() final;
    void dispose() final;
    const char* getSourceName() const final;
    BSONObjSet getOutputSorts() final;

    /**
     * Convenience method for creating a new $group stage.
     */
    static boost::intrusive_ptr<DocumentSourceGroup> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        std::vector<AccumulationStatement> accumulationStatements,
        Variables::Id numVariables,
        size_t maxMemoryUsageBytes = kDefaultMaxMemoryUsageBytes);

    /**
     * Parses 'elem' into a $group stage, or throws a UserException if 'elem' was an invalid
     * specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Add an accumulator, which will become a field in each Document that results from grouping.
     */
    void addAccumulator(AccumulationStatement accumulationStatement);

    /**
     * Sets the expression to use to determine the group id of each document.
     */
    void setIdExpression(const boost::intrusive_ptr<Expression> idExpression);

    /**
     * Tell this source if it is doing a merge from shards. Defaults to false.
     */
    void setDoingMerge(bool doingMerge) {
        _doingMerge = doingMerge;
    }

    bool isStreaming() const {
        return _streaming;
    }

    // Virtuals for SplittableDocumentSource.
    boost::intrusive_ptr<DocumentSource> getShardSource() final;
    boost::intrusive_ptr<DocumentSource> getMergeSource() final;

private:
    explicit DocumentSourceGroup(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                 size_t maxMemoryUsageBytes = kDefaultMaxMemoryUsageBytes);

    /**
     * getNext() dispatches to one of these three depending on what type of $group it is. All three
     * of these methods expect '_currentAccumulators' to have been reset before being called, and
     * also expect initialize() to have been called already.
     */
    GetNextResult getNextStreaming();
    GetNextResult getNextSpilled();
    GetNextResult getNextStandard();

    /**
     * Attempt to identify an input sort order that allows us to turn into a streaming $group. If we
     * find one, return it. Otherwise, return boost::none.
     */
    boost::optional<BSONObj> findRelevantInputSort() const;

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
    Value computeId(Variables* vars);

    /**
     * Converts the internal representation of the group key to the _id shape specified by the
     * user.
     */
    Value expandId(const Value& val);

    /**
     * 'vFieldName' contains the field names for the result documents, 'vpAccumulatorFactory'
     * contains the accumulator factories for the result documents, and 'vpExpression' contains the
     * common expressions used by each instance of each accumulator in order to find the right-hand
     * side of what gets added to the accumulator. These three vectors parallel each other.
     */
    std::vector<std::string> vFieldName;
    std::vector<Accumulator::Factory> vpAccumulatorFactory;
    std::vector<boost::intrusive_ptr<Expression>> vpExpression;

    bool _doingMerge;
    size_t _memoryUsageBytes = 0;
    size_t _maxMemoryUsageBytes;
    std::unique_ptr<Variables> _variables;
    std::vector<std::string> _idFieldNames;  // used when id is a document
    std::vector<boost::intrusive_ptr<Expression>> _idExpressions;

    BSONObj _inputSort;
    bool _streaming;
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
    const bool _extSortAllowed;

    std::pair<Value, Value> _firstPartOfNextGroup;
    // Only used when '_sorted' is true.
    boost::optional<Document> _firstDocOfNextGroup;
};

}  // namespace mongo
