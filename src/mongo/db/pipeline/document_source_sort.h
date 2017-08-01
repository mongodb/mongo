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

#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

class DocumentSourceSort final : public DocumentSource, public SplittableDocumentSource {
public:
    static const uint64_t kMaxMemoryUsageBytes = 100 * 1024 * 1024;

    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // A $sort does not modify any paths.
        return {GetModPathsReturn::Type::kFiniteSet, std::set<std::string>{}, {}};
    }

    bool canSwapWithMatch() const final {
        // Can't swap with a $match if a limit has been absorbed, since in general match can't swap
        // with limit.
        return !limitSrc;
    }

    BSONObjSet getOutputSorts() final {
        return allPrefixes(_rawSort);
    }

    GetDepsReturn getDependencies(DepsTracker* deps) const final;

    boost::intrusive_ptr<DocumentSource> getShardSource() final;
    boost::intrusive_ptr<DocumentSource> getMergeSource() final;

    /// Write out a Document whose contents are the sort key.
    Document serializeSortKey(bool explain) const;

    /**
     * Parses a $sort stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Convenience method for creating a $sort stage.
     */
    static boost::intrusive_ptr<DocumentSourceSort> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        BSONObj sortOrder,
        long long limit = -1,
        uint64_t maxMemoryUsageBytes = kMaxMemoryUsageBytes);

    /**
     * Returns -1 for no limit.
     */
    long long getLimit() const;

    /**
     * Loads a document to be sorted. This can be used to sort a stream of documents that are not
     * coming from another DocumentSource. Once all documents have been added, the caller must call
     * loadingDone() before using getNext() to receive the documents in sorted order.
     */
    void loadDocument(const Document& doc);

    /**
     * Signals to the sort stage that there will be no more input documents. It is an error to call
     * loadDocument() once this method returns.
     */
    void loadingDone();

    /**
     * Instructs the sort stage to use the given set of cursors as inputs, to merge documents that
     * have already been sorted.
     */
    void populateFromCursors(const std::vector<DBClientCursor*>& cursors);

    bool isPopulated() {
        return _populated;
    };

    boost::intrusive_ptr<DocumentSourceLimit> getLimitSrc() const {
        return limitSrc;
    }

protected:
    /**
     * Attempts to absorb a subsequent $limit stage so that it an perform a top-k sort.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;
    void doDispose() final;

private:
    // This is used to merge pre-sorted results from a DocumentSourceMergeCursors.
    class IteratorFromCursor;

    using MySorter = Sorter<Value, Document>;

    // For MySorter.
    class Comparator {
    public:
        explicit Comparator(const DocumentSourceSort& source) : _source(source) {}
        int operator()(const MySorter::Data& lhs, const MySorter::Data& rhs) const {
            return _source.compare(lhs.first, rhs.first);
        }

    private:
        const DocumentSourceSort& _source;
    };

    // Represents one of the components in a compound sort pattern. Each component is either the
    // field path by which we are sorting, or an Expression which can be used to retrieve the sort
    // value in the case of a $meta-sort (but not both).
    struct SortPatternPart {
        bool isAscending = true;
        boost::optional<FieldPath> fieldPath;
        boost::intrusive_ptr<Expression> expression;
    };

    using SortPattern = std::vector<SortPatternPart>;

    explicit DocumentSourceSort(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        MONGO_UNREACHABLE;  // Should call serializeToArray instead.
    }

    /**
     * Before returning anything, we have to consume all input and sort it. This method consumes all
     * input and prepares the sorted stream '_output'.
     *
     * This method may not be able to finish populating the sorter in a single call if 'pSource'
     * returns a DocumentSource::GetNextResult::kPauseExecution, so it returns the last
     * GetNextResult encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult populate();

    SortOptions makeSortOptions() const;

    /**
     * Returns the sort key for 'doc' based on the SortPattern. Attempts to generate the key using a
     * fast path that does not handle arrays. If an array is encountered, falls back on
     * extractKeyWithArray().
     */
    Value extractKey(const Document& doc) const;

    /**
     * Returns the sort key for 'doc' based on the SortPattern, or ErrorCodes::InternalError if an
     * array is encountered during sort key generation.
     */
    StatusWith<Value> extractKeyFast(const Document& doc) const;

    /**
     * Extracts the sort key component described by 'keyPart' from 'doc' and returns it. Returns
     * ErrorCodes::Internal error if the path for 'keyPart' contains an array in 'doc'.
     */
    StatusWith<Value> extractKeyPart(const Document& doc, const SortPatternPart& keyPart) const;

    /**
     * Returns the sort key for 'doc' based on the SortPattern.
     */
    Value extractKeyWithArray(const Document& doc) const;

    int compare(const Value& lhs, const Value& rhs) const;

    /**
     * Absorbs 'limit', enabling a top-k sort. It is safe to call this multiple times, it will keep
     * the smallest limit.
     */
    void setLimitSrc(boost::intrusive_ptr<DocumentSourceLimit> limit) {
        if (!limitSrc || limit->getLimit() < limitSrc->getLimit()) {
            limitSrc = limit;
        }
    }

    bool _populated = false;

    BSONObj _rawSort;

    boost::optional<SortKeyGenerator> _sortKeyGen;

    SortPattern _sortPattern;

    // The set of paths on which we're sorting.
    std::set<std::string> _paths;

    boost::intrusive_ptr<DocumentSourceLimit> limitSrc;

    uint64_t _maxMemoryUsageBytes;
    bool _done;
    bool _mergingPresorted;
    std::unique_ptr<MySorter> _sorter;
    std::unique_ptr<MySorter::Iterator> _output;
};

}  // namespace mongo
