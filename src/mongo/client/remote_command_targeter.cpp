/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace {

// This value is used if the operation doesn't have a user-specified max wait time. It should be
// closer to (preferably higher than) the replication electionTimeoutMillis in order to ensure that
// lack of primary due to replication election does not cause findHost failures.
const Seconds kDefaultFindHostMaxWaitTime(20);

// When calculating the findHost max wait time and the operation has a user-specified max wait time,
// pessimistially assume that the findHost would take this much time so that when it returns, there
// is still time left to complete the actual operation.
const Seconds kFindHostTimeoutPad(1);

}  // namespace

Milliseconds RemoteCommandTargeter::selectFindHostMaxWaitTime(OperationContext* txn) {
    // TODO: Get remaining max time from 'txn'.
    Milliseconds remainingMaxTime(0);
    if (remainingMaxTime > Milliseconds::zero()) {
        return std::min(remainingMaxTime - kFindHostTimeoutPad,
                        Milliseconds(kDefaultFindHostMaxWaitTime));
    }

    return kDefaultFindHostMaxWaitTime;
}

}  // namespace mongo
