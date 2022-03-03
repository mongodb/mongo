/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/user_write_block_mode_op_observer.h"

#include "mongo/db/s/global_user_write_block_state.h"

namespace mongo {

void UserWriteBlockModeOpObserver::onInserts(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const UUID& uuid,
                                             std::vector<InsertStatement>::const_iterator first,
                                             std::vector<InsertStatement>::const_iterator last,
                                             bool fromMigrate) {
    _checkWriteAllowed(opCtx, nss);
}

void UserWriteBlockModeOpObserver::onUpdate(OperationContext* opCtx,
                                            const OplogUpdateEntryArgs& args) {
    _checkWriteAllowed(opCtx, args.nss);
}

void UserWriteBlockModeOpObserver::onDelete(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const UUID& uuid,
                                            StmtId stmtId,
                                            const OplogDeleteEntryArgs& args) {
    _checkWriteAllowed(opCtx, nss);
}

void UserWriteBlockModeOpObserver::_checkWriteAllowed(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    GlobalUserWriteBlockState::get(opCtx)->checkUserWritesAllowed(opCtx, nss);
}

}  // namespace mongo
