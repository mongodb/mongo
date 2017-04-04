/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_fix_up_info.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/rollback_fix_up_info_descriptions.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {

namespace {

const auto kRollbackNamespacePrefix = "local.system.rollback."_sd;

}  // namespace

const NamespaceString RollbackFixUpInfo::kRollbackDocsNamespace(kRollbackNamespacePrefix + "docs");

const NamespaceString RollbackFixUpInfo::kRollbackCollectionUuidNamespace(kRollbackNamespacePrefix +
                                                                          "collectionUuid");

RollbackFixUpInfo::RollbackFixUpInfo(StorageInterface* storageInterface)
    : _storageInterface(storageInterface) {
    invariant(storageInterface);
}

Status RollbackFixUpInfo::processSingleDocumentOplogEntry(OperationContext* opCtx,
                                                          const UUID& collectionUuid,
                                                          const BSONElement& docId,
                                                          SingleDocumentOpType opType) {
    SingleDocumentOperationDescription desc(collectionUuid, docId, opType);
    return _upsertById(opCtx, kRollbackDocsNamespace, desc.toBSON());
}

Status RollbackFixUpInfo::processCreateCollectionOplogEntry(OperationContext* opCtx,
                                                            const UUID& collectionUuid) {
    // TODO: Remove references to this collection UUID from other rollback fix up info collections.

    CollectionUuidDescription desc(collectionUuid, {});
    return _upsertById(opCtx, kRollbackCollectionUuidNamespace, desc.toBSON());
}

Status RollbackFixUpInfo::processDropCollectionOplogEntry(OperationContext* opCtx,
                                                          const UUID& collectionUuid,
                                                          const NamespaceString& nss) {
    CollectionUuidDescription desc(collectionUuid, nss);
    return _upsertById(opCtx, kRollbackCollectionUuidNamespace, desc.toBSON());
}

Status RollbackFixUpInfo::processRenameCollectionOplogEntry(
    OperationContext* opCtx,
    const UUID& sourceCollectionUuid,
    const NamespaceString& sourceNss,
    boost::optional<CollectionUuidAndNss> targetCollectionUuidAndNss) {
    CollectionUuidDescription sourceDesc(sourceCollectionUuid, sourceNss);

    auto status = _upsertById(opCtx, kRollbackCollectionUuidNamespace, sourceDesc.toBSON());
    if (!status.isOK()) {
        return status;
    }

    // If target collection is not dropped during the rename operation, there is nothing further to
    // do.
    if (!targetCollectionUuidAndNss) {
        return Status::OK();
    }

    CollectionUuidDescription targetDesc(targetCollectionUuidAndNss->first,
                                         targetCollectionUuidAndNss->second);
    return _upsertById(opCtx, kRollbackCollectionUuidNamespace, targetDesc.toBSON());
}

Status RollbackFixUpInfo::_upsertById(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const BSONObj& update) {
    auto key = update["_id"];
    invariant(!key.eoo());
    return _storageInterface->upsertById(opCtx, nss, key, update);
}

}  // namespace repl
}  // namespace mongo
