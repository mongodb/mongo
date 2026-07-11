// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_params.h"

namespace mongo {

/**
 * A dummy test stage parameters class used for testing. It just allocates an ID.
 */
DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Test);

/**
 * A dummy LiteParsedDocumentSource that implements just enough functionality to test select
 * functionality.
 */
class TestLiteParsed final : public LiteParsedDocumentSourceDefault<TestLiteParsed> {
public:
    TestLiteParsed(
        const BSONElement& originalBson,
        FirstStageViewApplicationPolicy policy = FirstStageViewApplicationPolicy::kDefaultPrepend)
        : LiteParsedDocumentSourceDefault(originalBson), _policy(policy) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return {};
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<TestStageParams>(_originalBson);
    }

    FirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy() const override {
        return _policy;
    }

    FirstStageViewApplicationPolicy _policy;
};

}  // namespace mongo
