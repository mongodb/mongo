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

#include "mongo/db/exec/agg/list_local_sessions_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"

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
    StringData stageName,
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
