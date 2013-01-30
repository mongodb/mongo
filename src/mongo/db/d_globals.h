// @file d_globals.h

/**
*    Copyright (C) 2012 10gen Inc.
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

//
// these are global variables used in mongod ("d").  also used in test binary as that is effectively a variation on mongod code.
// that is, these are not in mongos.
//

#pragma once

namespace mongo { 

    class RWLockRecursive;
    class ClientCursorMonitor;

    struct DGlobals : boost::noncopyable { 
        DGlobals();

        // these are intentionally never deleted:
        ClientCursorMonitor& clientCursorMonitor;

    };

    extern DGlobals d;

};
