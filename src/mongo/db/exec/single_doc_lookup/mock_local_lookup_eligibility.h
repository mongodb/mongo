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
