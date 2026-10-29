// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_shard_server_info_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_shardserver_info.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<mongo::exec::agg::Stage> internalShardServerInfoStageToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSourceInternalShardServerInfo) {
    auto* ptr = dynamic_cast<DocumentSourceInternalShardServerInfo*>(
        documentSourceInternalShardServerInfo.get());
    tassert(10979900, "expected 'DocumentSourceInternalShardServerInfo' type", ptr);
    return make_intrusive<mongo::exec::agg::InternalShardServerInfoStage>(ptr->kStageName,
                                                                          ptr->getExpCtx());
}

namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(internalShardServerInfoStage,
                           DocumentSourceInternalShardServerInfo::id,
                           internalShardServerInfoStageToStageFn);

GetNextResult InternalShardServerInfoStage::doGetNext() {
    if (!_didEmit) {
        auto shardName =
            pExpCtx->getMongoProcessInterface()->getShardName(pExpCtx->getOperationContext());
        auto hostAndPort =
            pExpCtx->getMongoProcessInterface()->getHostAndPort(pExpCtx->getOperationContext());
        _didEmit = true;
        return GetNextResult(DOC("shard" << shardName << "host" << hostAndPort));
    }

    return GetNextResult::makeEOF();
}

}  // namespace exec::agg
}  // namespace mongo
