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

namespace mongo {

class DocumentSourceLimit final : public DocumentSource, public SplittableDocumentSource {
public:
    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    BSONObjSet getOutputSorts() final {
        return pSource ? pSource->getOutputSorts()
                       : SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    }

    /**
     * Attempts to combine with a subsequent $limit stage, setting 'limit' appropriately.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;
    Value serialize(bool explain = false) const final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return SEE_NEXT;  // This doesn't affect needed fields
    }

    /**
      Create a new limiting DocumentSource.

      @param pExpCtx the expression context for the pipeline
      @returns the DocumentSource
     */
    static boost::intrusive_ptr<DocumentSourceLimit> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    // Virtuals for SplittableDocumentSource
    // Need to run on rounter. Running on shard as well is an optimization.
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return this;
    }
    boost::intrusive_ptr<DocumentSource> getMergeSource() final {
        return this;
    }

    long long getLimit() const {
        return _limit;
    }
    void setLimit(long long newLimit) {
        _limit = newLimit;
    }

    /**
      Create a limiting DocumentSource from BSON.

      This is a convenience method that uses the above, and operates on
      a BSONElement that has been deteremined to be an Object with an
      element named $limit.

      @param pBsonElement the BSONELement that defines the limit
      @param pExpCtx the expression context
      @returns the grouping DocumentSource
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceLimit(const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    long long _limit;
    long long _nReturned = 0;
};

}  // namespace mongo
