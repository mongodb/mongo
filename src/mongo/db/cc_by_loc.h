/*    Copyright (C) 20012 10gen Inc.
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
 */

/**
 *  Defines CursorId and defines ByLocKey, which pairs up a DiskLoc
 *  and a CursorId and provides a comparison operation.
 */

#pragma once

#include <limits>
#include <map>

#include "mongo/db/diskloc.h"

namespace mongo {

    typedef long long CursorId; /* passed to the client so it can send back on getMore */
    static const CursorId INVALID_CURSOR_ID = -1; // But see SERVER-5726.

    // TODO(acm|mathias): ByLocKey allows constructing a map so we can
    // invalidate or advance all cursors pointing to a DiskLoc when we
    // remove that record. During review of moving this class to this
    // header, Mathias said: "On further thought, I think this need
    // would be better served by an unordered_map<DiskLoc,
    // unordered_map<CursorId, ClientCursor*>>." At which point, this
    // class should be removed.

    struct ByLocKey {

        ByLocKey(const DiskLoc& l, const CursorId& i) : loc(l), id(i) {}

        static ByLocKey min(const DiskLoc& l) {
            return ByLocKey(l, std::numeric_limits<long long>::min());
        }

        static ByLocKey max(const DiskLoc& l) {
            return ByLocKey(l, std::numeric_limits<long long>::max());
        }

        const DiskLoc loc;
        const CursorId id;
    };

    inline bool operator<(const ByLocKey& lhs, const ByLocKey& rhs) {
        int x = lhs.loc.compare(rhs.loc);
        if (x)
            return x < 0;
        return lhs.id < rhs.id;
    }

    class ClientCursor;
    typedef std::map<ByLocKey, ClientCursor*> CCByLoc;

} // namespace mongo
