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

/** replica set member */

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <ostream>
#include <string>
#include <type_traits>

namespace mongo {
namespace repl {

/**
 * Enumeration representing the state of a replica set member node. The State -> integer mappings
 * are persisted and/or sent over the network, so their values are reserved forever. Do not change
 * or delete them, except to update RS_MAX when introducing new states.
 */
struct MONGO_MOD_PUB MemberState {
    enum MS {
        // Server is still starting-up or the replica set is still being initiated
        RS_STARTUP = 0,

        // The node has won the latest election round and is running as primary, but is not
        // necessarily writable. I.e., it might still be draining oplog.
        RS_PRIMARY = 1,

        // The node is running as a secondary
        RS_SECONDARY = 2,

        // The node is either re-syncing after having failed off the end of the oplog or it just
        // finished an initial sync (or initial sync was not necessary). After that it usually
        // auto-transitions to secondary.
        RS_RECOVERING = 3,

        // This state doesn't exist since 3.0 and the value is here as a placeholder to avoid
        // accidental reuse of the ordinal.
        //
        // Indicates that an unrecovable error was encountered during replication state transition
        // and the repl subsystem is offline, but the server is still running. This has been
        // replaced with fassert since SERVER-15089.
        OBSOLETE_RS_FATAL = 4,

        // The node has loaded the replica set config document and is determining whether initial
        // sync is necessary. If initial sync is necessary it will be performed under the same
        // state, after which the node will to into RS_RECOVERING.
        RS_STARTUP2 = 5,

        // This value can only be returned for the remote node and never for the local member. Means
        // that the remote node was reachable, but either the connection establishment failed (e.g.,
        // for authentication reasons) or its state couldn't be retrieved due to error.
        RS_UNKNOWN = 6,

        // The node is running as an arbiter
        RS_ARBITER = 7,

        // This value can only be returned for the remote node and never for the local member. Means
        // that the remote node was not reachable.
        RS_DOWN = 8,

        // The node is running rollback
        RS_ROLLBACK = 9,

        // This value can only be returned for the local node and never for any of the remote
        // members. It means that this node does not consider itself to be part of the replica set,
        // which could be for one of two reasons:
        //  (1) The node was actually removed from the replica set, in which case node will not find
        //  its index in the config
        //  (2) The node was a part of a config server at some point in time in the past, but was
        //  restarted without the --configsvr option. This is a legacy reason to avoid customers
        //  inadvertently omitting that argument (and the skipShardingConfigurationChecks startup
        //  parameter).
        RS_REMOVED = 10,

        // Counts the rest. Update if a new value is added.
        RS_MAX = 10
    } s;

    template <typename T>
    static StatusWith<MemberState> create(T t) {
        MONGO_STATIC_ASSERT_MSG(std::is_integral<T>::value, "T must be an integral type");
        using UnderlyingType = std::underlying_type<MS>::type;
        if (std::is_signed<T>::value && (t < 0))
            return {ErrorCodes::BadValue, str::stream() << "MemberState cannot be negative"};
        auto asUnderlying = static_cast<UnderlyingType>(t);
        if (asUnderlying > RS_MAX)
            return {ErrorCodes::BadValue, str::stream() << "Unrecognized numerical state"};
        return {static_cast<MemberState>(asUnderlying)};
    }

    MemberState(MS ms = RS_UNKNOWN) : s(ms) {}
    explicit MemberState(int ms) : s((MS)ms) {}

    bool startup() const {
        return s == RS_STARTUP;
    }
    bool primary() const {
        return s == RS_PRIMARY;
    }
    bool secondary() const {
        return s == RS_SECONDARY;
    }
    bool recovering() const {
        return s == RS_RECOVERING;
    }
    bool startup2() const {
        return s == RS_STARTUP2;
    }
    bool rollback() const {
        return s == RS_ROLLBACK;
    }
    bool removed() const {
        return s == RS_REMOVED;
    }
    bool arbiter() const {
        return s == RS_ARBITER;
    }

    /**
     * The node is in a state where the data on disk is in a consistent state and any writes that
     * are incoming are either from user writes (primary) or from oplog application (secondary).
     */
    bool readable() const {
        return primary() || secondary();
    }

    std::string toString() const;

    bool operator==(const MemberState& r) const {
        return s == r.s;
    }
    bool operator!=(const MemberState& r) const {
        return s != r.s;
    }
};

inline std::string MemberState::toString() const {
    switch (s) {
        case RS_STARTUP:
            return "STARTUP";
        case RS_PRIMARY:
            return "PRIMARY";
        case RS_SECONDARY:
            return "SECONDARY";
        case RS_RECOVERING:
            return "RECOVERING";
        case OBSOLETE_RS_FATAL:  // Unreachable
            break;
        case RS_STARTUP2:
            return "STARTUP2";
        case RS_ARBITER:
            return "ARBITER";
        case RS_DOWN:
            return "DOWN";
        case RS_ROLLBACK:
            return "ROLLBACK";
        case RS_UNKNOWN:
            return "UNKNOWN";
        case RS_REMOVED:
            return "REMOVED";
    }
    MONGO_UNREACHABLE;
}

/**
 * Insertion operator for MemberState. Formats member state for output stream.
 * For testing only.
 */
MONGO_MOD_PUB inline std::ostream& operator<<(std::ostream& os, const MemberState& state) {
    return os << state.toString();
}

}  // namespace repl
}  // namespace mongo
