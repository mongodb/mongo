// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(InternalHybridSearch);

/**
 * Lite-parsed $_internalHybridSearch marker, appended (never at position 0) by the lite-parse
 * hybrid-search desugarer to the desugared pipeline and each $unionWith sub-pipeline. Enforces
 * canRunOnTimeseries=false at each collection acquisition.
 */
class LiteParsedInternalHybridSearch final
    : public LiteParsedDocumentSourceDefault<LiteParsedInternalHybridSearch> {
public:
    static constexpr std::string_view kStageName = "$_internalHybridSearch"sv;

    // Used by the desugarer, which synthesizes the (empty) spec itself.
    LiteParsedInternalHybridSearch()
        : LiteParsedDocumentSourceDefault<LiteParsedInternalHybridSearch>(
              BSON(kStageName << BSONObj())) {}

    explicit LiteParsedInternalHybridSearch(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<LiteParsedInternalHybridSearch>(originalBson) {}

    static std::unique_ptr<LiteParsedInternalHybridSearch> parse(const NamespaceString&,
                                                                 const BSONElement& spec,
                                                                 const LiteParserOptions&) {
        // Validate here: the StageParams registry path hands createFromBson this stage's
        // _originalBson, so discarding 'spec' would make the DocumentSource-level checks dead
        // code.
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << kStageName << " must take a nested object but found: " << spec,
                spec.type() == BSONType::object);
        uassert(ErrorCodes::FailedToParse,
                str::stream() << kStageName
                              << " must take an empty object but found: " << spec.embeddedObject(),
                spec.embeddedObject().isEmpty());
        return std::make_unique<LiteParsedInternalHybridSearch>(spec);
    }

    Constraints constraints() const override {
        return {.canRunOnTimeseries = false,
                .timeseriesUnsupportedStageName = "$rankFusion/$scoreFusion"sv};
    }

    FirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy() const override {
        // Never at position 0 by desugarer invariant, so view application never consults this.
        MONGO_UNREACHABLE_TASSERT(12109100);
    }

    std::unique_ptr<StageParams> getStageParams() const override {
        return std::make_unique<InternalHybridSearchStageParams>(_originalBson);
    }
};

}  // namespace mongo
