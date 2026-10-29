// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <deque>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * This class handles the execution part of the list catalog aggregation stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceListCatalog, which handles the optimization part.
 */
class ListCatalogStage final : public Stage {
public:
    ListCatalogStage(std::string_view stageName,
                     const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    GetNextResult doGetNext() final;

    boost::optional<std::deque<BSONObj>> _catalogDocs;
};
}  // namespace mongo::exec::agg
