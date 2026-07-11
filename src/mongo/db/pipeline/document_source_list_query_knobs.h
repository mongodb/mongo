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
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ListQueryKnobs);

/**
 * Internal aggregation stage that returns one document per registered query knob, exposing its
 * name, wire name, whether it is settable via Parameterized Query Settings, and type information
 * (with an allowedValues array for enum knobs). Implemented as a wrapper around
 * DocumentSourceQueue.
 */
class DocumentSourceListQueryKnobs final {
public:
    static constexpr std::string_view kStageName = "$listQueryKnobs";

    class LiteParsed : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(specElem);
        }

        LiteParsed(const BSONElement& spec) : LiteParsedDocumentSourceDefault(spec) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return {};
        }

        bool requiresAuthzChecks() const override {
            return false;
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<ListQueryKnobsStageParams>(_originalBson);
        }
    };

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

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
};

}  // namespace mongo
