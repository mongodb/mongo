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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"

#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace MONGO_MOD_NEEDS_REPLACEMENT mongo {

class UnionWithStageParams : public DefaultStageParams {
public:
    UnionWithStageParams(NamespaceString unionNss,
                         std::vector<BSONObj> pipeline,
                         bool hasForeignDB,
                         bool isHybridSearch,
                         // TODO SERVER-121262 Have the StageParams be owner of the BSONObj instead.
                         BSONElement originalBson,
                         boost::optional<LiteParsedPipeline> liteParsedPipeline = boost::none)
        : DefaultStageParams(originalBson),
          unionNss(std::move(unionNss)),
          pipeline(std::move(pipeline)),
          hasForeignDB(hasForeignDB),
          isHybridSearch(isHybridSearch),
          liteParsedPipeline(std::move(liteParsedPipeline)) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    NamespaceString unionNss;
    std::vector<BSONObj> pipeline;
    bool hasForeignDB;

    // TODO SERVER-121091 This can be removed once hybrid search desugars into the internal hybrid
    // search stage.
    bool isHybridSearch;

    // The desugared LiteParsedPipeline for the subpipeline. Present when the $unionWith spec
    // includes a pipeline in BSON object form, and absent when it includes string shorthand, i.e.
    // {$unionWith: "collName"}.
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
};

class LiteParsedUnionWith final
    : public LiteParsedDocumentSourceNestedPipelines<LiteParsedUnionWith> {
public:
    static constexpr StringData kStageName = "$unionWith"_sd;

    static std::unique_ptr<LiteParsedUnionWith> parse(const NamespaceString& nss,
                                                      const BSONElement& spec,
                                                      const LiteParserOptions& options);

    LiteParsedUnionWith(const BSONElement& spec,
                        NamespaceString foreignNss,
                        boost::optional<LiteParsedPipeline> pipeline,
                        std::vector<BSONObj> rawPipeline,
                        bool hasForeignDB,
                        bool isHybridSearch);

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final;

    bool requiresAuthzChecks() const override;

    std::unique_ptr<StageParams> getStageParams() const override;

    bool hasExtensionVectorSearchStage() const override;

    bool hasExtensionSearchStage() const override;

    static void validateUnionWithCollectionlessPipeline(
        const boost::optional<std::vector<mongo::BSONObj>>& pipeline);

private:
    std::vector<BSONObj> _rawPipeline;
    bool _hasForeignDB;
    bool _isHybridSearch;
};

}  // namespace MONGO_MOD_NEEDS_REPLACEMENT mongo
