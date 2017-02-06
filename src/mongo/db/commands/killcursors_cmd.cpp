/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/commands/killcursors_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/killcursors_request.h"

namespace mongo {

class KillCursorsCmd final : public KillCursorsCmdBase {
    MONGO_DISALLOW_COPYING(KillCursorsCmd);

public:
    KillCursorsCmd() = default;

private:
    Status _killCursor(OperationContext* txn, const NamespaceString& nss, CursorId cursorId) final {
        std::unique_ptr<AutoGetCollectionOrViewForRead> ctx;

        CursorManager* cursorManager;
        if (nss.isListIndexesCursorNS() || nss.isListCollectionsCursorNS()) {
            // listCollections and listIndexes are special cursor-generating commands whose cursors
            // are managed globally, as they operate over catalog data rather than targeting the
            // data within a collection.
            cursorManager = CursorManager::getGlobalCursorManager();
        } else {
            ctx = stdx::make_unique<AutoGetCollectionOrViewForRead>(txn, nss);
            Collection* collection = ctx->getCollection();
            ViewDefinition* view = ctx->getView();
            if (view) {
                Database* db = ctx->getDb();
                auto resolved = db->getViewCatalog()->resolveView(txn, nss);
                if (!resolved.isOK()) {
                    return resolved.getStatus();
                }
                ctx->releaseLocksForView();
                Status status = _killCursor(txn, resolved.getValue().getNamespace(), cursorId);
                {
                    // Set the namespace of the curop back to the view namespace so ctx records
                    // stats on this view namespace on destruction.
                    stdx::lock_guard<Client> lk(*txn->getClient());
                    CurOp::get(txn)->setNS_inlock(nss.ns());
                }
                return status;
            }
            if (!collection) {
                return {ErrorCodes::CursorNotFound,
                        str::stream() << "collection does not exist: " << nss.ns()};
            }
            cursorManager = collection->getCursorManager();
        }
        invariant(cursorManager);

        return cursorManager->eraseCursor(txn, cursorId, true /*shouldAudit*/);
    }
} killCursorsCmd;

}  // namespace mongo
