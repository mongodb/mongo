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

#pragma once

#include "mongo/db/diskloc.h"

namespace mongo {

    struct CollectionScanParams {
        enum Direction {
            FORWARD,
            BACKWARD,
        };

        CollectionScanParams() : start(DiskLoc()),
                                 direction(FORWARD),
                                 tailable(false) { }

        // What collection?
        string ns;

        // isNull by default.  If you specify any value for this, you're responsible for the DiskLoc
        // not being invalidated before the first call to work(...).
        DiskLoc start;

        Direction direction;

        // Do we want the scan to be 'tailable'?  Only meaningful if the collection is capped.
        bool tailable;
    };

}  // namespace mongo
