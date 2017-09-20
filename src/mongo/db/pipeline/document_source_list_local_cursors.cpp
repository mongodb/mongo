/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_list_local_cursors.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/generic_cursor_manager.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/pipeline/document_sources_gen.h"

namespace mongo {

REGISTER_TEST_DOCUMENT_SOURCE(listLocalCursors,
                              DocumentSourceListLocalCursors::LiteParsed::parse,
                              DocumentSourceListLocalCursors::createFromBson);

const char* DocumentSourceListLocalCursors::kStageName = "$listLocalCursors";

DocumentSource::GetNextResult DocumentSourceListLocalCursors::getNext() {
    pExpCtx->checkForInterrupt();

    if (!_cursors.empty()) {
        Document doc(_cursors.back().toBSON());
        _cursors.pop_back();
        return std::move(doc);
    }

    return GetNextResult::makeEOF();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceListLocalCursors::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {

    uassert(
        ErrorCodes::InvalidNamespace,
        str::stream() << kStageName
                      << " must be run against the database with {aggregate: 1}, not a collection",
        pExpCtx->ns.isCollectionlessAggregateNS());

    uassert(ErrorCodes::BadValue,
            str::stream() << kStageName << " must be run as { " << kStageName << ": {}}",
            spec.isABSONObj() && spec.Obj().isEmpty());

    return new DocumentSourceListLocalCursors(pExpCtx);
}

DocumentSourceListLocalCursors::DocumentSourceListLocalCursors(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx) {
    const auto& opCtx = pExpCtx->opCtx;
    _cursors = GenericCursorManager::get(opCtx)->getCursors(opCtx);
}
}
