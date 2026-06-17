/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
     * Lifecycle hooks invoked by the parent stage at batch boundaries. A strategy that caches
     * opCtx-bound resources must drop them in detachFromOperationContext(), since they do not
     * outlive the current opCtx.
     */
    virtual void detachFromOperationContext() {}
    virtual void reattachToOperationContext(OperationContext* opCtx) {}
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
        return _fallback->performLookup(expCtx, nss, collectionUUID, documentKey, afterClusterTime);
    }

    void detachFromOperationContext() override {
        _primary->detachFromOperationContext();
        _fallback->detachFromOperationContext();
    }

    void reattachToOperationContext(OperationContext* opCtx) override {
        _primary->reattachToOperationContext(opCtx);
        _fallback->reattachToOperationContext(opCtx);
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
