/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    // static
    bool WorkingSetCommon::fetch(WorkingSetMember* member) {
        // Already fetched.
        if (member->hasObj()) {
            // If we did a fetch we shouldn't have this around...
            verify(member->keyData.empty());
            return true;
        }

        if (!member->hasLoc()) { return false; }

        member->obj = member->loc.obj();
        member->state = WorkingSetMember::LOC_AND_OWNED_OBJ;

        // We have an obj. so get rid of the key data.
        member->keyData.clear();
        return true;
    }

    // static
    bool WorkingSetCommon::fetchAndInvalidateLoc(WorkingSetMember* member) {
        // Already in our desired state.
        if (member->state == WorkingSetMember::OWNED_OBJ) { return true; }

        if (fetch(member)) {
            member->obj = member->obj.getOwned();
            member->state = WorkingSetMember::OWNED_OBJ;
            member->loc = DiskLoc();
            return true;
        }
        else { return false; }
    }

}  // namespace mongo
