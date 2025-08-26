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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"

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
    InternalListCollectionsStage(StringData stageName,
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
