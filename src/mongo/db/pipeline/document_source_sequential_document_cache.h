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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/sequential_document_cache.h"

namespace mongo {

/**
 * A DocumentSourceSequentialDocumentCache manages an underlying SequentialDocumentCache. If the
 * cache's status is 'kBuilding', DocumentSourceSequentialDocumentCache will retrieve documents from
 * the preceding pipeline stage, add them to the cache, and pass them through to the following
 * pipeline stage. If the cache is in 'kServing' mode, DocumentSourceSequentialDocumentCache will
 * return results directly from the cache rather than from a preceding stage. It does not have a
 * registered parser and cannot be created from BSON.
 */
class DocumentSourceSequentialDocumentCache final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$sequentialCache"_sd;

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const {
        StageConstraints constraints(StreamType::kStreaming,
                                     _cache->isServing() ? PositionRequirement::kFirst
                                                         : PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed);

        constraints.requiresInputDocSource = (_cache->isBuilding());
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    GetNextResult getNext() final;

    static boost::intrusive_ptr<DocumentSourceSequentialDocumentCache> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, SequentialDocumentCache* cache) {
        return new DocumentSourceSequentialDocumentCache(pExpCtx, cache);
    }

    /**
     * Transitions the SequentialDocumentCache object's state to CacheStatus::kAbandoned. Once
     * abandoned it is expected that the cache will not be used for subsequent operations.
     */
    void abandonCache() {
        invariant(_cache);
        _cache->abandon();
    }

protected:
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    DocumentSourceSequentialDocumentCache(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          SequentialDocumentCache* cache);

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    SequentialDocumentCache* _cache;

    // This flag is set to prevent the cache stage from immediately serving from the cache after it
    // has exhausted input from its source for the first time.
    bool _cacheIsEOF = false;

    bool _hasOptimizedPos = false;
};

}  // namespace mongo
