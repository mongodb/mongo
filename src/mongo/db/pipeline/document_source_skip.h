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

class DocumentSourceSkip final : public DocumentSource, public NeedsMergerDocumentSource {
public:
    static constexpr StringData kStageName = "$skip"_sd;

    /**
     * Convenience method for creating a $skip stage.
     */
    static boost::intrusive_ptr<DocumentSourceSkip> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long nToSkip);

    /**
     * Parses the user-supplied BSON into a $skip stage.
     *
     * Throws a AssertionException if 'elem' is an invalid $skip specification.
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

    /**
     * Attempts to move a subsequent $limit before the skip, potentially allowing for forther
     * optimizations earlier in the pipeline.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    boost::intrusive_ptr<DocumentSource> optimize() final;
    BSONObjSet getOutputSorts() final {
        return pSource ? pSource->getOutputSorts()
                       : SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    }

    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return SEE_NEXT;  // This doesn't affect needed fields
    }

    // Virtuals for NeedsMergerDocumentSource
    // Need to run on rounter. Can't run on shards.
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return NULL;
    }
    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final {
        return {this};
    }

    long long getSkip() const {
        return _nToSkip;
    }
    void setSkip(long long newSkip) {
        _nToSkip = newSkip;
    }

private:
    explicit DocumentSourceSkip(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                long long nToSkip);

    long long _nToSkip = 0;
    long long _nSkippedSoFar = 0;
};

}  // namespace mongo
