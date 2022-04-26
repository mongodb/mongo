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

#pragma once

#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_sync_shared_data.h"

namespace mongo {
namespace repl {

enum class ResumePhase { kNone, kDataSync, kOplogCatchup };

class TenantMigrationSharedData final : public ReplSyncSharedData {
public:
    TenantMigrationSharedData(ClockSource* clock, const UUID& migrationId)
        : ReplSyncSharedData(clock), _migrationId(migrationId), _resumePhase(ResumePhase::kNone) {}
    TenantMigrationSharedData(ClockSource* clock, const UUID& migrationId, ResumePhase resumePhase)
        : ReplSyncSharedData(clock), _migrationId(migrationId), _resumePhase(resumePhase) {}

    void setLastVisibleOpTime(WithLock, OpTime opTime);

    OpTime getLastVisibleOpTime(WithLock);

    const mongo::UUID& getMigrationId() const {
        return _migrationId;
    }

    ResumePhase getResumePhase() const {
        return _resumePhase;
    }

private:
    // Must hold mutex (in base class) to access this.
    // Represents last visible majority committed donor opTime.
    OpTime _lastVisibleOpTime;

    // Id of the current tenant migration.
    const UUID _migrationId;

    // Indicate the phase from which the tenant migration is resuming due to recipient/donor
    // failovers.
    const ResumePhase _resumePhase;
};
}  // namespace repl
}  // namespace mongo
