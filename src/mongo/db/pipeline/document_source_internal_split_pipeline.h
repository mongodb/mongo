/**
 * Copyright (C) 2017 MongoDB Inc.
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

/**
 * An internal stage available for testing. Acts as a simple passthrough of intermediate results
 * from the source stage, but forces the pipeline to split at the point where this stage appears
 * (assuming that no earlier splitpoints exist). Takes a single parameter, 'mergeType', which can be
 * one of 'primaryShard', 'anyShard' or 'mongos' to control where the merge may occur. Omitting this
 * parameter or specifying 'mongos' produces the default merging behaviour; the merge half of the
 * pipeline will be executed on mongoS if all other stages are eligible, and will be sent to a
 * random participating shard otherwise.
 */
class DocumentSourceInternalSplitPipeline final : public DocumentSource,
                                                  public SplittableDocumentSource {
public:
    static constexpr StringData kStageName = "$_internalSplitPipeline"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, HostTypeRequirement mergeType) {
        return new DocumentSourceInternalSplitPipeline(expCtx, mergeType);
    }

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return nullptr;
    }

    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final {
        return {this};
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                _mergeType,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed};
    }

    GetNextResult getNext() final;

private:
    DocumentSourceInternalSplitPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        HostTypeRequirement mergeType)
        : DocumentSource(expCtx), _mergeType(mergeType) {}

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    HostTypeRequirement _mergeType = HostTypeRequirement::kNone;
};

}  // namesace mongo
