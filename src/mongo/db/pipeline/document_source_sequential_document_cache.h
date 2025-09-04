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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/sequential_document_cache.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <set>

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
    using SequentialDocumentCachePtr = std::shared_ptr<SequentialDocumentCache>;
    static constexpr StringData kStageName = "$sequentialCache"_sd;

    const char* getSourceName() const final {
        return DocumentSourceSequentialDocumentCache::kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     _cache->isServing() ? PositionRequirement::kFirst
                                                         : PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        if (_cache->isBuilding()) {
            constraints.requiresInputDocSource = true;
        } else {
            constraints.setConstraintsForNoInputSources();
        }
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    static boost::intrusive_ptr<DocumentSourceSequentialDocumentCache> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, SequentialDocumentCachePtr cache) {
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

    /**
     * The newly created DocumentSource will share a backing cache with the original DocumentSource
     * that is being cloned. It is expected that only one of the DocumentSourceSequentialCache
     * copies will be used, and therefore only one will actively be using the cache.
     */
    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const final {
        auto newStage = create(newExpCtx ? newExpCtx : getExpCtx(), _cache);
        // Keep the position flag so in case the containing pipeline is cloned post-optimization.
        newStage->_hasOptimizedPos = _hasOptimizedPos;
        return newStage;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    bool hasOptimizedPos() const {
        return _hasOptimizedPos;
    }

protected:
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

private:
    DocumentSourceSequentialDocumentCache(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          SequentialDocumentCachePtr cache);

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    SequentialDocumentCachePtr _cache;
    bool _hasOptimizedPos = false;

    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceSequentialDocumentCacheToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);
};

}  // namespace mongo
