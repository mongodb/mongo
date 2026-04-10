/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Tracks statistics for work related to authoritative collection metadata recovery.
 */
class MONGO_MOD_PARENT_PRIVATE AuthoritativeCollectionMetadataStatistics {
public:
    void report(BSONObjBuilder& builder) const {
        builder.append("recoverersCreated", _numberOfRecoverersCreated.get());
        builder.append("diskRecoveriesPerformed", _numberOfDiskRecoveries.get());
        builder.append("versionResolvedBeforeRecovery",
                       _numberOfVersionResolvedBeforeRecovery.get());
        builder.append("versionResolvedAfterRecovery", _numberOfVersionResolvedAfterRecovery.get());
        builder.append("postRecoveryWaitResolvedByConfigTime",
                       _numberOfPostRecoveryWaitResolvedByConfigTime.get());
        builder.append("postRecoveryWaitResolvedByVersionChange",
                       _numberOfPostRecoveryWaitResolvedByVersionChange.get());
    }

    void registerCreationOfRecoverer() {
        _numberOfRecoverersCreated.incrementRelaxed();
    }

    void registerDiskRecovery() {
        _numberOfDiskRecoveries.incrementRelaxed();
    }

    void registerVersionResolvedBeforeRecovery() {
        _numberOfVersionResolvedBeforeRecovery.incrementRelaxed();
    }

    void registerVersionResolvedAfterRecovery() {
        _numberOfVersionResolvedAfterRecovery.incrementRelaxed();
    }

    void registerPostRecoveryWaitResolvedByConfigTime() {
        _numberOfPostRecoveryWaitResolvedByConfigTime.incrementRelaxed();
    }

    void registerPostRecoveryWaitResolvedByVersionChange() {
        _numberOfPostRecoveryWaitResolvedByVersionChange.incrementRelaxed();
    }

private:
    Counter64 _numberOfRecoverersCreated;
    Counter64 _numberOfDiskRecoveries;
    Counter64 _numberOfVersionResolvedBeforeRecovery;
    Counter64 _numberOfVersionResolvedAfterRecovery;
    Counter64 _numberOfPostRecoveryWaitResolvedByConfigTime;
    Counter64 _numberOfPostRecoveryWaitResolvedByVersionChange;
};
}  // namespace mongo
