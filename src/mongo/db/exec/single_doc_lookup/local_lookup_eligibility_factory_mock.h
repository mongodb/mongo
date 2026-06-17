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
