/**
*    Copyright (C) 2008 10gen Inc.
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

#include "pch.h"
#include "../client.h"
#include "../../client/dbclient.h"
#include "rs.h"
#include "../oplogreader.h"

namespace mongo {

    void dropAllDatabasesExceptLocal();

    void ReplSetImpl::syncDoInitialSync() { 
        log() << "replSet syncDoInitialSync" << rsLog;

        OplogReader r;

        sethbmsg("initial sync drop all databases");
        dropAllDatabasesExceptLocal();
        sethbmsg("initial sync - not yet implemented");
    }

}
