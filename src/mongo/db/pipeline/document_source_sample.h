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
#include "mongo/db/pipeline/document_source_sort.h"

namespace mongo {

class DocumentSourceSample final : public DocumentSource, public SplittableDocumentSource {
public:
    static constexpr StringData kStageName = "$sample"_sd;

    GetNextResult getNext() final;
    const char* getSourceName() const final {
        return kStageName.rawData();
    }
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kBlocking,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kWritesTmpData,
                FacetRequirement::kAllowed};
    }

    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        return SEE_NEXT;
    }

    boost::intrusive_ptr<DocumentSource> getShardSource() final;
    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final;

    long long getSampleSize() const {
        return _size;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    explicit DocumentSourceSample(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    long long _size;

    // Uses a $sort stage to randomly sort the documents.
    boost::intrusive_ptr<DocumentSourceSort> _sortStage;
};

}  // namespace mongo
