// ephemeral_for_test_recovery_unit.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"

#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/log.h"

namespace mongo {

void EphemeralForTestRecoveryUnit::commitUnitOfWork() {
    try {
        for (Changes::iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }
        _changes.clear();
    } catch (...) {
        std::terminate();
    }

    // This ensures that the journal listener gets called on each commit.
    // SERVER-22575: Remove this once we add a generic mechanism to periodically wait
    // for durability.
    waitUntilDurable();
}

void EphemeralForTestRecoveryUnit::abortUnitOfWork() {
    try {
        for (Changes::reverse_iterator it = _changes.rbegin(), end = _changes.rend(); it != end;
             ++it) {
            ChangePtr change = *it;
            LOG(2) << "CUSTOM ROLLBACK " << demangleName(typeid(*change));
            change->rollback();
        }
        _changes.clear();
    } catch (...) {
        std::terminate();
    }
}

Status EphemeralForTestRecoveryUnit::setReadFromMajorityCommittedSnapshot() {
    if (!repl::getGlobalReplicationCoordinator()->isReplEnabled()) {
        return Status::OK();
    } else {
        return {ErrorCodes::CommandNotSupported,
                "Current storage engine does not support majority readConcerns"};
    }
}
}
