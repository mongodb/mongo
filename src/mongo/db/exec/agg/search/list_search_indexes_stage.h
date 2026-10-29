// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/search/search_query_view_spec_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <queue>
#include <string_view>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class ListSearchIndexesStage final : public Stage {
public:
    ListSearchIndexesStage(std::string_view stageName,
                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                           BSONObj cmdObj);

private:
    GetNextResult doGetNext() final;

    BSONObj _cmdObj;
    std::queue<BSONObj> _searchIndexes;
    bool _eof = false;
    // Cache the collection UUID to avoid retrieving the collection UUID for each 'doGetNext'
    // call.
    boost::optional<UUID> _collectionUUID;
    // Cache the underlying source collection name, which is necessary for supporting running
    // search queries on views, to avoid retrieving on each getNext.
    boost::optional<NamespaceString> _resolvedNamespace;
    // Cache the view for search queries on views.
    boost::optional<SearchQueryViewSpec> _view;
};

}  // namespace mongo::exec::agg
