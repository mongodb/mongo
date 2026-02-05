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

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"

namespace mongo {

/**
 * StageParams for DocumentSourceInternalSearchIdLookUp. This class encapsulates the parameters
 * needed to construct a DocumentSourceInternalSearchIdLookUp stage.
 */
class InternalSearchIdLookupStageParams : public StageParams {
public:
    InternalSearchIdLookupStageParams() = default;
    InternalSearchIdLookupStageParams(long long limit,
                                      boost::optional<LiteParsedPipeline> viewPipeline)
        : limit(limit), viewPipeline(std::move(viewPipeline)) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    const long long limit;
    const boost::optional<LiteParsedPipeline> viewPipeline;
};

class LiteParsedInternalSearchIdLookUp final
    : public LiteParsedDocumentSourceDefault<LiteParsedInternalSearchIdLookUp> {
public:
    static std::unique_ptr<LiteParsedInternalSearchIdLookUp> parse(const NamespaceString& nss,
                                                                   const BSONElement& spec,
                                                                   const LiteParserOptions&) {
        uassert(ErrorCodes::FailedToParse,
                "$_internalSearchIdLookup specification must be an object",
                spec.type() == BSONType::object);
        return std::make_unique<LiteParsedInternalSearchIdLookUp>(spec.wrap().getOwned());
    }

    bool isInitialSource() const override {
        return false;
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<InternalSearchIdLookupStageParams>(
            (*_ownedBson)["$_internalSearchIdLookup"]["limit"].safeNumberLong(), _viewPipeline);
    }

    const BSONObj& getBsonSpec() const {
        return *_ownedBson;
    }

    ViewPolicy getViewPolicy() const final {
        return ViewPolicy{.policy = ViewPolicy::kFirstStageApplicationPolicy::kDoNothing,
                          .callback = [this](const ViewInfo& viewInfo, StringData) {
                              _viewPipeline = viewInfo.getViewPipeline();
                          }};
    }

    LiteParsedInternalSearchIdLookUp(BSONObj spec)
        : LiteParsedDocumentSourceDefault(spec.getOwned()) {}

private:
    mutable boost::optional<LiteParsedPipeline> _viewPipeline;
};

}  // namespace mongo
