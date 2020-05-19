/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_server_status.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_kv_engine.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_radix_store.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace ephemeral_for_test {

ServerStatusSection::ServerStatusSection(KVEngine* engine)
    : mongo::ServerStatusSection(kEngineName), _engine(engine) {}

bool ServerStatusSection::includeByDefault() const {
    return true;
}

BSONObj ServerStatusSection::generateSection(OperationContext* opCtx,
                                             const BSONElement& configElement) const {
    Lock::GlobalLock lk(
        opCtx, LockMode::MODE_IS, Date_t::now(), Lock::InterruptBehavior::kLeaveUnlocked);
    if (!lk.isLocked()) {
        LOGV2_DEBUG(4919800, 2, "Failed to retrieve ephemeralForTest statistics");
        return BSONObj();
    }

    BSONObjBuilder bob;
    bob.append("totalMemoryUsage", StringStore::totalMemory());
    bob.append("totalNodes", StringStore::totalNodes());
    bob.append("averageChildren", StringStore::averageChildren());

    return bob.obj();
}

}  // namespace ephemeral_for_test
}  // namespace mongo
