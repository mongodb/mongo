/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_index_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/server_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/net/socket_utils.h"

#include <iterator>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(indexStats,
                         DocumentSourceIndexStats::LiteParsed::parse,
                         DocumentSourceIndexStats::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);


// Implements 'DocumentSourceIndexStats' based on a shard-only 'DocumentSourceQueue' stage.
intrusive_ptr<DocumentSource> DocumentSourceIndexStats::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(28803,
            "The $indexStats stage specification must be an empty object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());

    // Get the index stats for the current shard and map them over a deferred queue. The queue won't
    // be populated until reaching the shards due to the host type requirement.
    DocumentSourceQueue::DeferredQueue deferredQueue{[pExpCtx]() {
        auto indexStats = pExpCtx->getMongoProcessInterface()->getIndexStats(
            pExpCtx->getOperationContext(),
            pExpCtx->getNamespaceString(),
            prettyHostNameAndPort(pExpCtx->getOperationContext()->getClient()->getLocalPort()),
            !serverGlobalParams.clusterRole.has(ClusterRole::None));
        std::deque<DocumentSource::GetNextResult> queue;
        std::copy(std::make_move_iterator(indexStats.begin()),
                  std::make_move_iterator(indexStats.end()),
                  std::back_inserter(queue));
        return queue;
    }};

    // Since the deferred queue needs to be initialized only on shards, the default
    // 'DocumentSourceQueue::serialize()' method needs to be avoided, so a 'serializeOverride' is
    // provided. Without this, 'DocumentSourceQueue::serialize()' will trigger the deferred queue
    // initialization on 'mongos' instances, leading to 'MONGO_UNREACHEABLE'.
    return make_intrusive<DocumentSourceQueue>(std::move(deferredQueue),
                                               pExpCtx,
                                               /* stageNameOverride */ kStageName,
                                               /* serializeOverride*/ Value{elem.wrap()},
                                               /* constraintsOverride */ constraints());
}
}  // namespace mongo
