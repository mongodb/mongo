/**
 * Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/db/free_mon/free_mon_storage.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

static const NamespaceString adminSystemVersionNss("admin.system.version");
constexpr auto kFreeMonDocIdKey = "free_monitoring";

// mms-automation stores its document in local.clustermanager
static const NamespaceString localClusterManagerNss("local.clustermanager");

}  // namespace

boost::optional<FreeMonStorageState> FreeMonStorage::read(OperationContext* opCtx) {
    BSONObj deleteKey = BSON("_id" << kFreeMonDocIdKey);
    BSONElement elementKey = deleteKey.firstElement();

    auto storageInterface = repl::StorageInterface::get(opCtx);

    Lock::DBLock dblk(opCtx, adminSystemVersionNss.db(), MODE_IS);
    Lock::CollectionLock lk(opCtx->lockState(), adminSystemVersionNss.ns(), MODE_IS);

    auto swObj = storageInterface->findById(opCtx, adminSystemVersionNss, elementKey);
    if (!swObj.isOK()) {
        if (swObj.getStatus() == ErrorCodes::NoSuchKey) {
            return {};
        }

        uassertStatusOK(swObj.getStatus());
    }

    return FreeMonStorageState::parse(IDLParserErrorContext("FreeMonStorage"), swObj.getValue());
}

void FreeMonStorage::replace(OperationContext* opCtx, const FreeMonStorageState& doc) {
    BSONObj deleteKey = BSON("_id" << kFreeMonDocIdKey);
    BSONElement elementKey = deleteKey.firstElement();

    BSONObj obj = doc.toBSON();

    auto storageInterface = repl::StorageInterface::get(opCtx);
    {
        Lock::DBLock dblk(opCtx, adminSystemVersionNss.db(), MODE_IS);
        Lock::CollectionLock lk(opCtx->lockState(), adminSystemVersionNss.ns(), MODE_IS);

        auto swObj = storageInterface->upsertById(opCtx, adminSystemVersionNss, elementKey, obj);
        if (!swObj.isOK()) {
            uassertStatusOK(swObj);
        }
    }
}

void FreeMonStorage::deleteState(OperationContext* opCtx) {
    BSONObj deleteKey = BSON("_id" << kFreeMonDocIdKey);
    BSONElement elementKey = deleteKey.firstElement();

    auto storageInterface = repl::StorageInterface::get(opCtx);
    {
        Lock::DBLock dblk(opCtx, adminSystemVersionNss.db(), MODE_IS);
        Lock::CollectionLock lk(opCtx->lockState(), adminSystemVersionNss.ns(), MODE_IS);

        auto swObj = storageInterface->deleteById(opCtx, adminSystemVersionNss, elementKey);
        if (!swObj.isOK()) {
            // Ignore errors about no document
            if (swObj.getStatus() == ErrorCodes::NoSuchKey) {
                return;
            }

            uassertStatusOK(swObj);
        }
    }
}

boost::optional<BSONObj> FreeMonStorage::readClusterManagerState(OperationContext* opCtx) {
    auto storageInterface = repl::StorageInterface::get(opCtx);

    Lock::DBLock dblk(opCtx, adminSystemVersionNss.db(), MODE_IS);
    Lock::CollectionLock lk(opCtx->lockState(), adminSystemVersionNss.ns(), MODE_IS);

    auto swObj = storageInterface->findSingleton(opCtx, localClusterManagerNss);
    if (!swObj.isOK()) {
        // Ignore errors about not-finding documents or having too many documents
        if (swObj.getStatus() == ErrorCodes::NamespaceNotFound ||
            swObj.getStatus() == ErrorCodes::CollectionIsEmpty ||
            swObj.getStatus() == ErrorCodes::TooManyMatchingDocuments) {
            return {};
        }

        uassertStatusOK(swObj.getStatus());
    }

    return swObj.getValue();
}

void FreeMonStorage::getStatus(OperationContext* opCtx, BSONObjBuilder* builder) {
    auto state = read(opCtx);
    if (!state) {
        builder->append("state", "undecided");
        return;
    }

    builder->append("state", StorageState_serializer(state->getState()));
    builder->append("message", state->getMessage());
    builder->append("url", state->getInformationalURL());
    builder->append("userReminder", state->getUserReminder());
}

}  // namespace mongo
