// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace mongo::exec::agg {

/**
 * Test double for LocalLookupEligibility. Drives specific eligibility outcomes in lookup-strategy
 * unit tests without spinning up Grid / CatalogCache / ShardingState.
 *
 * Two usage modes:
 *
 *   1. Constant outcome via convenience factories:
 *        MockLocalLookupEligibility::makeAlwaysLocal()
 *        MockLocalLookupEligibility::makeAlwaysUnknown()
 *        MockLocalLookupEligibility::makeAlwaysLocalWithVersion(sv, dv)
 *
 *   2. Programmable callback for parameterised tests:
 *        auto mock = std::make_unique<MockLocalLookupEligibility>(
 *            [&](const boost::intrusive_ptr<ExpressionContext>&,
 *                const NamespaceString&,
 *                const Document&) { return capturedDecision; });
 *
 * Every run() call is recorded in calls() so tests can assert call count and arguments without
 * EXPECT_CALL ceremony. The mock does not route, so it invokes the body exactly once with the
 * programmed decision.
 */
class MockLocalLookupEligibility final : public LocalLookupEligibility {
public:
    using Callback = std::function<Decision(
        const boost::intrusive_ptr<ExpressionContext>&, const NamespaceString&, const Document&)>;

    struct Call {
        NamespaceString nss;
        Document documentKey;
    };

    explicit MockLocalLookupEligibility(Callback cb) : _cb(std::move(cb)) {}

    const std::vector<Call>& calls() const {
        return _calls;
    }

    size_t callCount() const {
        return _calls.size();
    }

    void resetCalls() {
        _calls.clear();
    }

    // ---- Convenience factories ----

    static std::unique_ptr<MockLocalLookupEligibility> makeAlwaysLocal() {
        return std::make_unique<MockLocalLookupEligibility>(
            [](const boost::intrusive_ptr<ExpressionContext>&,
               const NamespaceString&,
               const Document&) -> Decision { return Local{}; });
    }

    static std::unique_ptr<MockLocalLookupEligibility> makeAlwaysUnknown() {
        return std::make_unique<MockLocalLookupEligibility>(
            [](const boost::intrusive_ptr<ExpressionContext>&,
               const NamespaceString&,
               const Document&) -> Decision { return Unknown{}; });
    }

    static std::unique_ptr<MockLocalLookupEligibility> makeAlwaysLocalWithVersion(
        boost::optional<ShardVersion> sv, boost::optional<DatabaseVersion> dv = boost::none) {
        return std::make_unique<MockLocalLookupEligibility>(
            [sv = std::move(sv), dv = std::move(dv)](const boost::intrusive_ptr<ExpressionContext>&,
                                                     const NamespaceString&,
                                                     const Document&) -> Decision {
                return Local{.shardVersion = sv, .dbVersion = dv};
            });
    }

    SingleDocumentLookupExecutor::LookupResult run(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey,
        const AcquisitionState& acquisitionState,
        function_ref<SingleDocumentLookupExecutor::LookupResult(const Decision&)> body)
        const override {
        _calls.push_back(Call{nss, documentKey});
        return body(_cb(expCtx, nss, documentKey));
    }

private:
    Callback _cb;
    mutable std::vector<Call> _calls;
};

}  // namespace mongo::exec::agg
