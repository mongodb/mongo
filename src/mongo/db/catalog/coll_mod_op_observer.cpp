/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/catalog/coll_mod_op_observer.h"

#include "mongo/db/catalog/coll_mod_write_ops_tracker.h"

namespace mongo {

void CollModOpObserver::onInserts(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const UUID& uuid,
                                  std::vector<InsertStatement>::const_iterator begin,
                                  std::vector<InsertStatement>::const_iterator end,
                                  bool fromMigrate) {
    auto opsTracker = CollModWriteOpsTracker::get(opCtx->getServiceContext());
    for (auto it = begin; it != end; it++) {
        const InsertStatement& insertStmt = *it;
        opsTracker->onDocumentChanged(uuid, insertStmt.doc);
    }
}

void CollModOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    if (!args.updateArgs) {
        return;
    }
    auto opsTracker = CollModWriteOpsTracker::get(opCtx->getServiceContext());
    opsTracker->onDocumentChanged(args.uuid, args.updateArgs->updatedDoc);
}

}  // namespace mongo
