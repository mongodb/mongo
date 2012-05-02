/** @file common.cpp 
    Common code for server binaries (mongos, mongod, test).  
    Nothing used by driver should be here. 
 */

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

#include "jsobjmanipulator.h"

/**
 * this just has globals
 */
namespace mongo {

    NOINLINE_DECL OpTime OpTime::skewed() {
        bool toLog = false;
        ONCE toLog = true;
        RARELY toLog = true;
        last.i++;
        if ( last.i & 0x80000000 )
            toLog = true;
        if ( toLog ) {
            log() << "clock skew detected  prev: " << last.secs << " now: " << (unsigned) time(0) << endl;
        }
        if ( last.i & 0x80000000 ) {
            log() << "error large clock skew detected, shutting down" << endl;
            throw ClockSkewException();
        }
        return last;
    }

}
