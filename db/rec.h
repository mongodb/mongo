// rec.h
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
 */


/* TODO for _RECSTORE

   _ support > 2GB data per file
   _ multiple files, not just indexes.dat
   _ lazier writes? (may be done?)
   _ configurable cache size
   _ fix on abnormal terminations to be able to restart some
*/

#pragma once

namespace mongo { 

    //NamespaceDetails* nsdetails_notinline(const char *ns);

    /* todo: this is WRONG it doesn't include the RecordHeader's size */
    const int BucketSize = 8192;

    inline BtreeBucket* DiskLoc::btree() const {
        assert( _a != -1 );
        return (BtreeBucket *) rec()->data;
    }

}
