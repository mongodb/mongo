/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/db/repl/topology_coordinator.h"

#include <string>

#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {
namespace {
static const int kLeaderValue = 0;
static const int kFollowerValue = 1;
static const int kCandidateValue = 2;
}  // namespace

const TopologyCoordinator::Role TopologyCoordinator::Role::leader(kLeaderValue);
const TopologyCoordinator::Role TopologyCoordinator::Role::follower(kFollowerValue);
const TopologyCoordinator::Role TopologyCoordinator::Role::candidate(kCandidateValue);

TopologyCoordinator::Role::Role(int value) : _value(value) {}

std::string TopologyCoordinator::Role::toString() const {
    switch (_value) {
        case kLeaderValue:
            return "leader";
        case kFollowerValue:
            return "follower";
        case kCandidateValue:
            return "candidate";
    }
    invariant(false);
}

TopologyCoordinator::~TopologyCoordinator() {}

std::ostream& operator<<(std::ostream& os, TopologyCoordinator::Role role) {
    return os << role.toString();
}

}  // namespace repl
}  // namespace mongo
