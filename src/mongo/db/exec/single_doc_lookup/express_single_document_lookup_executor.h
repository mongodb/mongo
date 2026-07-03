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

#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/query/plan_summary_stats.h"

#include <memory>

namespace mongo::exec::agg {

/**
 * Fast-path lookup strategy: serves local _id point lookups via the Express executor.
 *
 * Eligibility-gated by LocalLookupEligibility: it serves the lookup only when the document is
 * decided local, otherwise it declines (kNotHandled) so the caller's fallback strategy handles it.
 * See performLookup() for the result-status contract.
 */
class ExpressSingleDocumentLookupExecutor : public SingleDocumentLookupExecutor {
public:
    ExpressSingleDocumentLookupExecutor(std::unique_ptr<CollectionAcquirer> collectionAcquirer,
                                        std::unique_ptr<LocalLookupEligibility> localEligibility)
        : _collectionAcquirer(std::move(collectionAcquirer)),
          _localEligibility(std::move(localEligibility)) {
        tassert(12841300,
                "ExpressSingleDocumentLookupExecutor requires a non-null collection acquirer",
                _collectionAcquirer);
        tassert(12841301,
                "ExpressSingleDocumentLookupExecutor requires a non-null eligibility policy",
                _localEligibility);
    }

    /**
     * Runs the lookup body under LocalLookupEligibility::run(), which supplies the Decision and (in
     * the routing-aware impl) owns catalog-and-routing refresh + retry. On a Local decision the
     * body installs ScopedSetShardRole(sv, dv), acquires the collection, and runs an Express _id
     * plan. Returns:
     *   - kNotHandled: Unknown decision, no _id in the documentKey, or no Express _id plan.
     *   - kDocumentFound: the document, read locally under the installed shard role.
     *   - kDocumentNotFound: _id absent, or the namespace / collection UUID no longer exists
     *     (matching master's null-fullDocument DDL semantic).
     *
     * Catalog-and-routing errors (StaleConfig / StaleDbVersion / ShardCannotRefreshDueToLocksHeld)
     * are not caught here: they propagate so the eligibility's CollectionRouter::route() can
     * refresh and retry.
     */
    LookupResult performLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const NamespaceString& nss,
                               boost::optional<UUID> collectionUUID,
                               const Document& documentKey,
                               boost::optional<Timestamp> afterClusterTime) override;

    void setPlanSummaryStatsSink(PlanSummaryStats* sink) override {
        _planSummaryStatsSink = sink;
    }

private:
    std::unique_ptr<CollectionAcquirer> _collectionAcquirer;
    std::unique_ptr<LocalLookupEligibility> _localEligibility;

    // Non-owning; owned by the caller. Stats are accumulated here when set.
    PlanSummaryStats* _planSummaryStatsSink = nullptr;
};

}  // namespace mongo::exec::agg
