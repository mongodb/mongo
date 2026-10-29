// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * This class handles the execution part of the internal list collections aggregation stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceInternalListCollections, which handles the optimization part.
 */
class InternalListCollectionsStage final : public Stage {
public:
    InternalListCollectionsStage(std::string_view stageName,
                                 const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                 const boost::intrusive_ptr<DocumentSourceMatch>& absorbedMatch);

private:
    GetNextResult doGetNext() final;

    void _buildCollectionsToReplyForDb(const DatabaseName& db,
                                       std::vector<BSONObj>& collectionsToReply);

    // A $match stage can be absorbed in order to avoid unnecessarily computing the databases
    // that do not match that predicate.
    boost::intrusive_ptr<DocumentSourceMatch> _absorbedMatch;

    boost::optional<std::vector<DatabaseName>> _databases;
    std::vector<BSONObj> _collectionsToReply;
};
}  // namespace mongo::exec::agg
