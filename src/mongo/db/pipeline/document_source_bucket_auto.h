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

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/granularity_rounder.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

/**
 * The $bucketAuto stage takes a user-specified number of buckets and automatically determines
 * boundaries such that the values are approximately equally distributed between those buckets.
 */
class DocumentSourceBucketAuto final : public DocumentSource, public SplittableDocumentSource {
public:
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    GetDepsReturn getDependencies(DepsTracker* deps) const final;
    GetNextResult getNext() final;
    const char* getSourceName() const final;

    /**
     * The $bucketAuto stage must be run on the merging shard.
     */
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return nullptr;
    }
    boost::intrusive_ptr<DocumentSource> getMergeSource() final {
        return this;
    }

    static const uint64_t kDefaultMaxMemoryUsageBytes = 100 * 1024 * 1024;

    /**
     * Convenience method to create a $bucketAuto stage.
     *
     * If 'accumulationStatements' is the empty vector, it will be filled in with the statement
     * 'count: {$sum: 1}'.
     */
    static boost::intrusive_ptr<DocumentSourceBucketAuto> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        int numBuckets,
        std::vector<AccumulationStatement> accumulationStatements = {},
        const boost::intrusive_ptr<GranularityRounder>& granularityRounder = nullptr,
        uint64_t maxMemoryUsageBytes = kDefaultMaxMemoryUsageBytes);

    /**
     * Parses a $bucketAuto stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

protected:
    void doDispose() final;

private:
    DocumentSourceBucketAuto(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                             const boost::intrusive_ptr<Expression>& groupByExpression,
                             int numBuckets,
                             std::vector<AccumulationStatement> accumulationStatements,
                             const boost::intrusive_ptr<GranularityRounder>& granularityRounder,
                             uint64_t maxMemoryUsageBytes);

    // struct for holding information about a bucket.
    struct Bucket {
        Bucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
               Value min,
               Value max,
               const std::vector<AccumulationStatement>& accumulationStatements);
        Value _min;
        Value _max;
        std::vector<boost::intrusive_ptr<Accumulator>> _accums;
    };

    /**
     * Consumes all of the documents from the source in the pipeline and sorts them by their
     * 'groupBy' value. This method might not be able to finish populating the sorter in a single
     * call if 'pSource' returns a DocumentSource::GetNextResult::kPauseExecution, so this returns
     * the last GetNextResult encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult populateSorter();

    /**
     * Computes the 'groupBy' expression value for 'doc'.
     */
    Value extractKey(const Document& doc);

    /**
     * Calculates the bucket boundaries for the input documents and places them into buckets.
     */
    void populateBuckets();

    /**
     * Adds the document in 'entry' to 'bucket' by updating the accumulators in 'bucket'.
     */
    void addDocumentToBucket(const std::pair<Value, Document>& entry, Bucket& bucket);

    /**
     * Adds 'newBucket' to _buckets and updates any boundaries if necessary.
     */
    void addBucket(Bucket& newBucket);

    /**
     * Makes a document using the information from bucket. This is what is returned when getNext()
     * is called.
     */
    Document makeDocument(const Bucket& bucket);

    std::unique_ptr<Sorter<Value, Document>> _sorter;
    std::unique_ptr<Sorter<Value, Document>::Iterator> _sortedInput;

    std::vector<AccumulationStatement> _accumulatedFields;

    int _nBuckets;
    uint64_t _maxMemoryUsageBytes;
    bool _populated = false;
    std::vector<Bucket> _buckets;
    std::vector<Bucket>::iterator _bucketsIterator;
    boost::intrusive_ptr<Expression> _groupByExpression;
    boost::intrusive_ptr<GranularityRounder> _granularityRounder;
    long long _nDocuments = 0;
};

}  // namespace mongo
