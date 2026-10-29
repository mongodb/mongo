// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <list>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Bucket);

/**
 * The $bucket stage is an alias for a $group stage followed by a $sort stage.
 */
class DocumentSourceBucket final {
public:
    static constexpr std::string_view kStageName = "$bucket"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(spec);
        }
        explicit LiteParsed(const BSONElement& spec) : LiteParsedDocumentSourceDefault(spec) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return {};
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return {};
        }

        bool requiresAuthzChecks() const override {
            return false;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<BucketStageParams>(_originalBson);
        }

        /**
         * The correct collation for the aggregate is needed at parse time to determine whether the
         * parsed bucket boundaries are valid.
         */
        bool requiresCollationForParsingUnshardedAggregate() const final {
            return true;
        }
    };
    /**
     * Returns a $group stage followed by a $sort stage.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // It is illegal to construct a DocumentSourceBucket directly, use createFromBson() instead.
    DocumentSourceBucket() = default;
};

}  // namespace mongo
