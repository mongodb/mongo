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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/working_set_common.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/service_context.h"

namespace mongo {

void WorkingSetCommon::prepareForSnapshotChange(WorkingSet* workingSet) {
    for (auto id : workingSet->getAndClearYieldSensitiveIds()) {
        if (workingSet->isFree(id)) {
            continue;
        }

        // We may see the same member twice, so anything we do here should be idempotent.
        WorkingSetMember* member = workingSet->get(id);
        if (member->getState() == WorkingSetMember::RID_AND_IDX) {
            member->isSuspicious = true;
        }
    }
}

// static
bool WorkingSetCommon::fetch(OperationContext* opCtx,
                             WorkingSet* workingSet,
                             WorkingSetID id,
                             unowned_ptr<SeekableRecordCursor> cursor) {
    WorkingSetMember* member = workingSet->get(id);

    // We should have a RecordId but need to retrieve the obj. Get the obj now and reset all WSM
    // state appropriately.
    invariant(member->hasRecordId());

    member->obj.reset();
    auto record = cursor->seekExact(member->recordId);
    if (!record) {
        return false;
    }

    member->obj = {opCtx->recoveryUnit()->getSnapshotId(), record->data.releaseToBson()};

    if (member->isSuspicious) {
        // Make sure that all of the keyData is still valid for this copy of the document.
        // This ensures both that index-provided filters and sort orders still hold.
        // TODO provide a way for the query planner to opt out of this checking if it is
        // unneeded due to the structure of the plan.
        invariant(!member->keyData.empty());
        for (size_t i = 0; i < member->keyData.size(); i++) {
            KeyStringSet keys;
            // There's no need to compute the prefixes of the indexed fields that cause the index to
            // be multikey when ensuring the keyData is still valid.
            KeyStringSet* multikeyMetadataKeys = nullptr;
            MultikeyPaths* multikeyPaths = nullptr;
            auto* iam = member->keyData[i].index;
            iam->getKeys(member->obj.value(),
                         IndexAccessMethod::GetKeysMode::kEnforceConstraints,
                         &keys,
                         multikeyMetadataKeys,
                         multikeyPaths,
                         member->recordId);
            KeyString::HeapBuilder keyString(iam->getSortedDataInterface()->getKeyStringVersion(),
                                             member->keyData[i].keyData,
                                             iam->getSortedDataInterface()->getOrdering(),
                                             member->recordId);
            if (!keys.count(keyString.release())) {
                // document would no longer be at this position in the index.
                return false;
            }
        }

        member->isSuspicious = false;
    }

    member->keyData.clear();
    workingSet->transitionToRecordIdAndObj(id);
    return true;
}

// static
BSONObj WorkingSetCommon::buildMemberStatusObject(const Status& status) {
    BSONObjBuilder bob;
    bob.append("ok", status.isOK() ? 1.0 : 0.0);
    bob.append("code", status.code());
    bob.append("errmsg", status.reason());
    if (auto extraInfo = status.extraInfo()) {
        extraInfo->serialize(&bob);
    }

    return bob.obj();
}

// static
WorkingSetID WorkingSetCommon::allocateStatusMember(WorkingSet* ws, const Status& status) {
    invariant(ws);

    WorkingSetID wsid = ws->allocate();
    WorkingSetMember* member = ws->get(wsid);
    member->obj = Snapshotted<BSONObj>(SnapshotId(), buildMemberStatusObject(status));
    member->transitionToOwnedObj();

    return wsid;
}

// static
bool WorkingSetCommon::isValidStatusMemberObject(const BSONObj& obj) {
    return obj.hasField("ok") && obj["code"].type() == NumberInt && obj["errmsg"].type() == String;
}

// static
void WorkingSetCommon::getStatusMemberObject(const WorkingSet& ws,
                                             WorkingSetID wsid,
                                             BSONObj* objOut) {
    invariant(objOut);

    // Validate ID and working set member.
    if (WorkingSet::INVALID_ID == wsid) {
        return;
    }
    WorkingSetMember* member = ws.get(wsid);
    if (!member->hasOwnedObj()) {
        return;
    }
    BSONObj obj = member->obj.value();
    if (!isValidStatusMemberObject(obj)) {
        return;
    }
    *objOut = obj;
}

// static
Status WorkingSetCommon::getMemberObjectStatus(const BSONObj& memberObj) {
    invariant(WorkingSetCommon::isValidStatusMemberObject(memberObj));
    return Status(ErrorCodes::Error(memberObj["code"].numberInt()),
                  memberObj["errmsg"].valueStringData(),
                  memberObj);
}

// static
Status WorkingSetCommon::getMemberStatus(const WorkingSetMember& member) {
    invariant(member.hasObj());
    return getMemberObjectStatus(member.obj.value());
}

// static
std::string WorkingSetCommon::toStatusString(const BSONObj& obj) {
    if (!isValidStatusMemberObject(obj)) {
        Status unknownStatus(ErrorCodes::UnknownError, "no details available");
        return unknownStatus.toString();
    }
    return getMemberObjectStatus(obj).toString();
}

}  // namespace mongo
