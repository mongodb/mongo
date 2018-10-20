
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/replication_lock_manager_manipulator.h"

namespace mongo {

ReplicationLockManagerManipulator::ReplicationLockManagerManipulator(LockManager* lockManager)
    : _lockManager(lockManager) {}

void ReplicationLockManagerManipulator::lockUncontestedTemporaryGlobalResource(
    LockManager::TemporaryResourceQueue* tempGlobalResource, LockRequest* request, LockMode mode) {
    // Sanity check that requests are not being reused without proper cleanup
    invariant(request->status == LockRequest::STATUS_NEW);
    invariant(request->recursiveCount == 1);
    invariant(!request->partitioned);
    invariant(tempGlobalResource->_lockHead.resourceId.getType() == ResourceType::RESOURCE_GLOBAL);
    invariant(mode == MODE_IX,
              str::stream() << "Locking temporary global resource must happen in MODE_IX, found: "
                            << mode);

    request->mode = mode;
    const auto lockResult = tempGlobalResource->_lockHead.newRequest(request);
    invariant(lockResult == LockResult::LOCK_OK);
}

void ReplicationLockManagerManipulator::replaceGlobalLocksWithLocksFromTemporaryGlobalResource(
    ResourceId resId, LockManager::TemporaryResourceQueue* tempGlobalResource) {
    invariant(resId.getType() == ResourceType::RESOURCE_GLOBAL);
    invariant(tempGlobalResource->_lockHead.resourceId == resId);

    LockManager::LockBucket* bucket = _lockManager->_getBucket(resId);
    stdx::lock_guard<SimpleMutex> scopedLock(bucket->mutex);
    LockHead* trueGlobalLockHead = bucket->findOrInsert(resId);

    LockHead* tempGlobalLockHead = &tempGlobalResource->_lockHead;

    invariant(trueGlobalLockHead->grantedCounts[MODE_X] == 1);
    invariant(trueGlobalLockHead->compatibleFirstCount == 1);
    invariant(tempGlobalLockHead->conflictList.empty());

    LockRequest* existingGlobalLockRequest = trueGlobalLockHead->grantedList._front;
    invariant(!existingGlobalLockRequest->next);
    invariant(existingGlobalLockRequest->mode == MODE_X);
    invariant(existingGlobalLockRequest->status == LockRequest::Status::STATUS_GRANTED);

    // Remove the existing granted MODE_X lock from the trueGlobalLockHead so it can be replaced
    // by the locks from tempGlobalLockHead.
    trueGlobalLockHead->grantedList.remove(existingGlobalLockRequest);
    trueGlobalLockHead->decGrantedModeCount(existingGlobalLockRequest->mode);
    trueGlobalLockHead->compatibleFirstCount--;

    // Now iterate over the granted LockRequests in the tempGlobalLockHead and transfer them over
    // to the trueGlobalLockHead.
    for (LockRequest* it = tempGlobalLockHead->grantedList._front; it != nullptr;) {
        LockRequest* next = it->next;

        invariant(it->mode == MODE_IX,
                  str::stream() << "Expected granted requests from temporary global resource to be "
                                   "in MODE_IX but found: "
                                << it->mode);
        invariant(it->status == LockRequest::Status::STATUS_GRANTED);
        invariant(it->lock == tempGlobalLockHead);

        it->lock = trueGlobalLockHead;
        tempGlobalLockHead->grantedList.remove(it);
        tempGlobalLockHead->decGrantedModeCount(it->mode);
        trueGlobalLockHead->grantedList.push_back(it);
        trueGlobalLockHead->incGrantedModeCount(it->mode);

        it = next;
    }
    invariant(tempGlobalLockHead->grantedList.empty());
    invariant(tempGlobalLockHead->grantedCounts[MODE_X] == 0);
    invariant(tempGlobalLockHead->grantedModes == 0);

    // Grant any pending requests against the true global lock head that can proceed now that the
    // global X lock has been released.
    _lockManager->_onLockModeChanged(trueGlobalLockHead, true);
}

}  // namespace mongo
