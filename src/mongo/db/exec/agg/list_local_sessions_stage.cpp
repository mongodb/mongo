// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/list_local_sessions_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"

#include <string_view>

namespace mongo::exec::agg {

boost::intrusive_ptr<exec::agg::Stage> documentSourceListLocalSessionsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* listLocalSessionsDS =
        dynamic_cast<DocumentSourceListLocalSessions*>(documentSource.get());

    tassert(10816800, "expected 'DocumentSourceListLocalSessions' type", listLocalSessionsDS);

    return make_intrusive<exec::agg::ListLocalSessionsStage>(listLocalSessionsDS->kStageName,
                                                             listLocalSessionsDS->getExpCtx(),
                                                             listLocalSessionsDS->getSpec());
}

REGISTER_AGG_STAGE_MAPPING(listLocalSessions,
                           DocumentSourceListLocalSessions::id,
                           documentSourceListLocalSessionsToStageFn);

ListLocalSessionsStage::ListLocalSessionsStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    const ListSessionsSpec& spec)
    : Stage(stageName, pExpCtx), _spec{spec} {
    const auto& opCtx = pExpCtx->getOperationContext();
    _cache = LogicalSessionCache::get(opCtx);
    if (_spec.getAllUsers()) {
        invariant(!_spec.getUsers() || _spec.getUsers()->empty());
        _ids = _cache->listIds();
    } else {
        _ids = _cache->listIds(listSessionsUsersToDigests(_spec.getUsers().value()));
    }
}

GetNextResult ListLocalSessionsStage::doGetNext() {
    while (!_ids.empty()) {
        const auto& id = _ids.back();
        const auto record = _cache->peekCached(id);
        _ids.pop_back();
        if (!record) {
            // It's possible for SessionRecords to have expired while we're walking
            continue;
        }
        return Document(record->toBSON());
    }

    return GetNextResult::makeEOF();
}
}  // namespace mongo::exec::agg
