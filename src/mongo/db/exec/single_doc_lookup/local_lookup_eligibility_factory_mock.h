// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility_factory_interface.h"
#include "mongo/db/exec/single_doc_lookup/mock_local_lookup_eligibility.h"

#include <functional>
#include <memory>
#include <utility>

namespace mongo::exec::agg {

/**
 * Test double for LocalLookupEligibilityFactoryInterface. Lets unit tests inject a specific
 * eligibility outcome (e.g. AlwaysUnknown) into a DocumentSource that builds a
 * SingleDocumentLookupExecutor chain, without spinning up Grid / CatalogCache / ShardingState or
 * relying on the production ShardingState dispatch.
 *
 * Two usage modes:
 *
 *   1. Constant outcome via convenience factories:
 *        LocalLookupEligibilityFactoryMock::makeAlwaysLocal()
 *        LocalLookupEligibilityFactoryMock::makeAlwaysUnknown()
 *
 *   2. Programmable callback:
 *        auto mock = std::make_unique<LocalLookupEligibilityFactoryMock>(
 *            [&](OperationContext*) { return std::make_unique<MyMockEligibility>(); });
 */
class LocalLookupEligibilityFactoryMock final : public LocalLookupEligibilityFactoryInterface {
public:
    using Callback = std::function<std::unique_ptr<LocalLookupEligibility>(OperationContext*)>;

    explicit LocalLookupEligibilityFactoryMock(Callback cb) : _cb(std::move(cb)) {}

    std::unique_ptr<LocalLookupEligibility> makeLocalLookupEligibility(
        OperationContext* opCtx) const override {
        return _cb(opCtx);
    }

    // ---- Convenience factories ----

    static std::unique_ptr<LocalLookupEligibilityFactoryMock> makeAlwaysLocal() {
        return std::make_unique<LocalLookupEligibilityFactoryMock>(
            [](OperationContext*) -> std::unique_ptr<LocalLookupEligibility> {
                return MockLocalLookupEligibility::makeAlwaysLocal();
            });
    }

    static std::unique_ptr<LocalLookupEligibilityFactoryMock> makeAlwaysUnknown() {
        return std::make_unique<LocalLookupEligibilityFactoryMock>(
            [](OperationContext*) -> std::unique_ptr<LocalLookupEligibility> {
                return MockLocalLookupEligibility::makeAlwaysUnknown();
            });
    }

private:
    Callback _cb;
};

}  // namespace mongo::exec::agg
