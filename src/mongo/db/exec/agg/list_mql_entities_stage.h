/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_list_mql_entities.h"
#include "mongo/db/pipeline/document_source_list_mql_entities_gen.h"
#include "mongo/db/pipeline/expression_context.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class ListMqlEntitiesStage final : public Stage {
public:
    ListMqlEntitiesStage(StringData stageName,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         MqlEntityTypeEnum type);

    static boost::intrusive_ptr<Stage> create_forTest(
        StringData stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        MqlEntityTypeEnum type,
        const StringMap<DocumentSource::ParserRegistration>& parserRegistrationMap);

private:
    static constexpr auto kEntityTypeFieldName =
        DocumentSourceListMqlEntities::kEntityTypeFieldName;

    ListMqlEntitiesStage(
        StringData stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        MqlEntityTypeEnum type,
        const StringMap<DocumentSource::ParserRegistration>& parserRegistrationMap);

    GetNextResult doGetNext() final;

    std::vector<std::string> _results;
    MqlEntityTypeEnum _type;
};

}  // namespace mongo::exec::agg
