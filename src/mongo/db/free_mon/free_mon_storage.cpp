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

#include "mongo/db/free_mon/free_mon_storage.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

// mms-automation stores its document in local.clustermanager
static const NamespaceString localClusterManagerNss("local.clustermanager");

}  // namespace

constexpr StringData FreeMonStorage::kFreeMonDocIdKey;

boost::optional<FreeMonStorageState> FreeMonStorage::read(OperationContext* opCtx) {
    BSONObj deleteKey = BSON("_id" << kFreeMonDocIdKey);
    BSONElement elementKey = deleteKey.firstElement();

    auto storageInterface = repl::StorageInterface::get(opCtx);

    // Ensure we read without a timestamp.
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              opCtx->recoveryUnit()->getTimestampReadSource());

    AutoGetCollectionForRead autoRead(opCtx, NamespaceString::kServerConfigurationNamespace);

    auto swObj = storageInterface->findById(
        opCtx, NamespaceString::kServerConfigurationNamespace, elementKey);
    if (!swObj.isOK()) {
        if (swObj.getStatus() == ErrorCodes::NoSuchKey ||
            swObj.getStatus() == ErrorCodes::NamespaceNotFound) {
            return {};
        }

        uassertStatusOK(swObj.getStatus());
    }

    return FreeMonStorageState::parse(IDLParserContext("FreeMonStorage"), swObj.getValue());
}

void FreeMonStorage::replace(OperationContext* opCtx, const FreeMonStorageState& doc) {
    BSONObj deleteKey = BSON("_id" << kFreeMonDocIdKey);
    BSONElement elementKey = deleteKey.firstElement();

    BSONObj obj = doc.toBSON();

    auto storageInterface = repl::StorageInterface::get(opCtx);
    AutoGetCollection autoWrite(opCtx, NamespaceString::kServerConfigurationNamespace, MODE_IX);

    if (repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(
            opCtx, NamespaceString::kServerConfigurationNamespace)) {
        auto swObj = storageInterface->upsertById(
            opCtx, NamespaceString::kServerConfigurationNamespace, elementKey, obj);
        if (!swObj.isOK()) {
            uassertStatusOK(swObj);
        }
    }
}

void FreeMonStorage::deleteState(OperationContext* opCtx) {
    BSONObj deleteKey = BSON("_id" << kFreeMonDocIdKey);
    BSONElement elementKey = deleteKey.firstElement();

    auto storageInterface = repl::StorageInterface::get(opCtx);
    AutoGetCollection autoWrite(opCtx, NamespaceString::kServerConfigurationNamespace, MODE_IX);

    if (repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(
            opCtx, NamespaceString::kServerConfigurationNamespace)) {

        auto swObj = storageInterface->deleteById(
            opCtx, NamespaceString::kServerConfigurationNamespace, elementKey);
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

    AutoGetCollectionForRead autoRead(opCtx, NamespaceString::kServerConfigurationNamespace);

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

}  // namespace mongo
