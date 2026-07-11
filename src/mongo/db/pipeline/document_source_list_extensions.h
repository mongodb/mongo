// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ListExtensions);

/**
 * Document source for the $listExtensions stage, implemented as a wrapper of DocumentSourceQueue.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceListExtensions final {
public:
    static constexpr std::string_view kStageName = "$listExtensions"sv;

    class LiteParsed : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(specElem);
        }

        LiteParsed(const BSONElement& spec)
            : LiteParsedDocumentSourceDefault(spec),
              _privileges({Privilege(ResourcePattern::forClusterResource(boost::none),
                                     ActionType::listExtensions)}) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return _privileges;
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<ListExtensionsStageParams>(_originalBson);
        }

    private:
        const PrivilegeVector _privileges;
    };

    /**
     * Returns the stage constraints used to override DocumentSourceQueue.
     */
    static StageConstraints constraints() {
        auto constraints = StageConstraints{DocumentSource::StreamType::kStreaming,
                                            DocumentSource::PositionRequirement::kFirst,
                                            DocumentSource::HostTypeRequirement::kReceivingHostOnly,
                                            DocumentSource::DiskUseRequirement::kNoDiskUse,
                                            DocumentSource::FacetRequirement::kNotAllowed,
                                            DocumentSource::TransactionRequirement::kNotAllowed,
                                            DocumentSource::LookupRequirement::kNotAllowed,
                                            DocumentSource::UnionRequirement::kNotAllowed};
        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    /**
     * Creates a DocumentSourceQueue using a DeferredQueue which lazily computes the loaded
     * extensions during the first getNext() call.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
};

}  // namespace mongo
