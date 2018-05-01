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

class DocumentSourceLimit final : public DocumentSource, public NeedsMergerDocumentSource {
public:
    static constexpr StringData kStageName = "$limit"_sd;

    /**
     * Create a new $limit stage.
     */
    static boost::intrusive_ptr<DocumentSourceLimit> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    /**
     * Parse a $limit stage from a BSON stage specification. 'elem's field name must be "$limit".
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed};
    }

    GetNextResult getNext() final;
    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    BSONObjSet getOutputSorts() final {
        return pSource ? pSource->getOutputSorts()
                       : SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    }

    /**
     * Attempts to combine with a subsequent $limit stage, setting 'limit' appropriately.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return SEE_NEXT;  // This doesn't affect needed fields
    }

    /**
     * Returns the current DocumentSourceLimit for use in the shards pipeline. Running this stage on
     * the shards is an optimization, but is not strictly necessary in order to produce correct
     * pipeline output.
     */
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return this;
    }

    /**
     * Returns a new DocumentSourceLimit with the same limit as the current stage, for use in the
     * merge pipeline. Unlike the shards source, it is necessary for this stage to run on the
     * merging host in order to produce correct pipeline output.
     */
    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final {
        return {DocumentSourceLimit::create(pExpCtx, _limit)};
    }

    long long getLimit() const {
        return _limit;
    }
    void setLimit(long long newLimit) {
        _limit = newLimit;
    }

private:
    DocumentSourceLimit(const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    long long _limit;
    long long _nReturned = 0;
};

}  // namespace mongo
