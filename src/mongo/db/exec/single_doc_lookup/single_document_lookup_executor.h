// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
struct PlanSummaryStats;
}  // namespace mongo

namespace mongo::exec::agg {

/**
 * Strategy interface for a single-document lookup by _id, used for change stream post-image
 * retrieval, $search idLookup, and similar point lookups. A strategy either handles a lookup or
 * declines it (kNotHandled), letting the caller fall back to another strategy.
 */
class SingleDocumentLookupExecutor {
public:
    virtual ~SingleDocumentLookupExecutor() = default;

    struct LookupResult {
        enum class HandledStatus {
            kNotHandled,        // Strategy declined; the caller should try a fallback.
            kDocumentNotFound,  // Strategy ran; the document does not exist.
            kDocumentFound,     // Strategy ran; the document was found.
        };
        HandledStatus status = HandledStatus::kNotHandled;
        boost::optional<Document> document;  // Set only when status is kDocumentFound.
    };

    /**
     * Performs a single-document lookup by _id.
     *
     * 'afterClusterTime', when set, is the clusterTime of the change event being enriched; the
     * lookup must observe the document at or after it. Absent for non-change-stream callers.
     *
     * Returns whether the strategy handled the lookup and, if so, whether the document was found.
     */
    [[nodiscard]] virtual LookupResult performLookup(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        boost::optional<UUID> collectionUUID,
        const Document& documentKey,
        boost::optional<Timestamp> afterClusterTime) = 0;

    /**
     * Opt-in: when set, each lookup's PlanSummaryStats (docs/keysExamined, indexesUsed, ...) are
     * accumulated into 'sink'. Strategies that surface execution stats override this; the default
     * is a no-op for those that don't (e.g. change-stream updateLookup).
     */
    virtual void setPlanSummaryStatsSink(PlanSummaryStats* sink) {}

    /**
     * Releases attached catalog state (collection acquisition handle, open storage cursors). Any
     * cached plan survives. Non-throwing.
     * Must be idempotent: it may be called more than once on the same executor.
     */
    virtual void releaseResources() noexcept {}
};

/**
 * Composes a primary strategy with a fallback: runs the primary, and on kNotHandled runs the
 * fallback. Lets a fast strategy opt out to a universal one for cases it cannot handle.
 */
class PrimaryWithFallbackSingleDocumentLookupExecutor : public SingleDocumentLookupExecutor {
public:
    PrimaryWithFallbackSingleDocumentLookupExecutor(
        std::unique_ptr<SingleDocumentLookupExecutor> primary,
        std::unique_ptr<SingleDocumentLookupExecutor> fallback)
        : _primary(std::move(primary)), _fallback(std::move(fallback)) {
        tassert(12840800, "primary executor must not be null", _primary);
        tassert(12840801, "fallback executor must not be null", _fallback);
    }

    LookupResult performLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const NamespaceString& nss,
                               boost::optional<UUID> collectionUUID,
                               const Document& documentKey,
                               boost::optional<Timestamp> afterClusterTime) override {
        auto result =
            _primary->performLookup(expCtx, nss, collectionUUID, documentKey, afterClusterTime);
        if (result.status != LookupResult::HandledStatus::kNotHandled) {
            return result;
        }

        // Release any resources before running the fallback executor.
        _primary->releaseResources();
        return _fallback->performLookup(expCtx, nss, collectionUUID, documentKey, afterClusterTime);
    }

    void setPlanSummaryStatsSink(PlanSummaryStats* sink) override {
        _primary->setPlanSummaryStatsSink(sink);
        _fallback->setPlanSummaryStatsSink(sink);
    }

    void releaseResources() noexcept override {
        _primary->releaseResources();
        _fallback->releaseResources();
    }

    /**
     * Test-only: expose the composed primary / fallback so wiring tests can assert the
     * concrete strategy types installed by stage_fn factories via dynamic_cast.
     */
    const SingleDocumentLookupExecutor* primary_forTest() const {
        return _primary.get();
    }
    const SingleDocumentLookupExecutor* fallback_forTest() const {
        return _fallback.get();
    }

private:
    std::unique_ptr<SingleDocumentLookupExecutor> _primary;
    std::unique_ptr<SingleDocumentLookupExecutor> _fallback;
};

}  // namespace mongo::exec::agg
