// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

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
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

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
    static constexpr std::string_view kStageName = "$sequentialCache"sv;

    std::string_view getSourceName() const final {
        return DocumentSourceSequentialDocumentCache::kStageName;
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
            constraints.preservesCardinality = true;
            constraints.requiresInputDocSource = true;
        } else {
            constraints.setConstraintsForNoInputSources();
        }
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
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
        tassert(11282967, "Missing document cache", _cache);
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

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

private:
    DocumentSourceSequentialDocumentCache(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          SequentialDocumentCachePtr cache);

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    SequentialDocumentCachePtr _cache;
    bool _hasOptimizedPos = false;

    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceSequentialDocumentCacheToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);
};

}  // namespace mongo
