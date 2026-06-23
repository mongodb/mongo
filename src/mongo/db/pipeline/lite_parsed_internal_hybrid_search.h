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

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(InternalHybridSearch);

/**
 * Lite-parsed $_internalHybridSearch marker, appended (never at position 0) by the lite-parse
 * hybrid-search desugarer to the desugared pipeline and each $unionWith sub-pipeline. Enforces
 * canRunOnTimeseries=false at each collection acquisition.
 */
class LiteParsedInternalHybridSearch final
    : public LiteParsedDocumentSourceDefault<LiteParsedInternalHybridSearch> {
public:
    static constexpr StringData kStageName = "$_internalHybridSearch"_sd;

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
                .timeseriesUnsupportedStageName = "$rankFusion/$scoreFusion"_sd};
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
