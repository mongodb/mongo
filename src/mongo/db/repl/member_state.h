/*
 *    Copyright (C) 2010 10gen Inc.
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

/** replica set member */

#pragma once

#include <string>
#include <type_traits>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {


/*
    RS_STARTUP    serving still starting up, or still trying to initiate the set
    RS_PRIMARY    this server thinks it is primary
    RS_SECONDARY  this server thinks it is a secondary (slave mode)
    RS_RECOVERING recovering/resyncing; after recovery usually auto-transitions to secondary
    RS_STARTUP2   loaded config, still determining who is primary

    State -> integer mappings are reserved forever.  Do not change them or delete them, except
    to update RS_MAX when introducing new states.
*/
struct MemberState {
    enum MS {
        RS_STARTUP = 0,
        RS_PRIMARY = 1,
        RS_SECONDARY = 2,
        RS_RECOVERING = 3,
        RS_STARTUP2 = 5,
        RS_UNKNOWN = 6, /* remote node not yet reached */
        RS_ARBITER = 7,
        RS_DOWN = 8, /* node not reachable for a report */
        RS_ROLLBACK = 9,
        RS_REMOVED = 10, /* node removed from replica set */
        RS_MAX = 10
    } s;

    template <typename T>
    static StatusWith<MemberState> create(T t) {
        static_assert(std::is_integral<T>::value, "T must be an integral type");
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
    bool readable() const {
        return s == RS_PRIMARY || s == RS_SECONDARY;
    }
    bool removed() const {
        return s == RS_REMOVED;
    }
    bool arbiter() const {
        return s == RS_ARBITER;
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
    return "";
}

}  // namespace repl
}  // namespace mongo
