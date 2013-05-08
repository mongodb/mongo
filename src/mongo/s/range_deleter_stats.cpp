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

#include "mongo/s/range_deleter_stats.h"

namespace mongo {
    const BSONField<int> RangeDeleterStats::TotalDeletesField("totalDeletes");
    const BSONField<int> RangeDeleterStats::PendingDeletesField("pendingDeletes");
    const BSONField<int> RangeDeleterStats::InProgressDeletesField("inProgressDeletes");

    BSONObj RangeDeleterStats::toBSON() const {
        scoped_lock sl(*_lockPtr);

        BSONObjBuilder builder;
        builder << TotalDeletesField(_totalDeletes);
        builder << PendingDeletesField(_pendingDeletes);
        builder << InProgressDeletesField(_inProgressDeletes);

        return builder.obj();
    }

    // Note: If we ever to decide to expose the other individual stats as well, we have
    // to remind the caller that calling them individually is never guaranteed to have a
    // consistent view of the stats. So toBSON should be used instead if the caller needs
    // a snapshot view of the state on more than one property.
    int RangeDeleterStats::getCurrentDeletes() const {
        scoped_lock sl(*_lockPtr);
        return _totalDeletes;
    }
}
