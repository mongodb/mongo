// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_list_mql_entities.h"
#include "mongo/db/pipeline/document_source_list_mql_entities_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class ListMqlEntitiesStage final : public Stage {
public:
    ListMqlEntitiesStage(std::string_view stageName,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         MqlEntityTypeEnum type);
    /**
     * TODO SERVER-112711: Remove '[[MONGO_MOD_PRIVATE]]' once
     * 'document_source_list_mql_entities_test.cpp' is split in two.
     */
    [[MONGO_MOD_PRIVATE]] static boost::intrusive_ptr<Stage> create_forTest(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        MqlEntityTypeEnum type,
        const LiteParsedDocumentSource::ParserMap& parserRegistrationMap);

private:
    static constexpr auto kEntityTypeFieldName =
        DocumentSourceListMqlEntities::kEntityTypeFieldName;

    ListMqlEntitiesStage(std::string_view stageName,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         MqlEntityTypeEnum type,
                         const LiteParsedDocumentSource::ParserMap& parserRegistrationMap);

    GetNextResult doGetNext() final;

    std::vector<std::string> _results;
    MqlEntityTypeEnum _type;
};

}  // namespace mongo::exec::agg
