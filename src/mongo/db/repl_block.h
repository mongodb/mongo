// repl_block.h - blocking on writes for replication

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

#pragma once

#include "../pch.h"
#include "client.h"
#include "curop.h"

/**
   local.slaves  - current location for all slaves

 */
namespace mongo {

    void updateSlaveLocation( CurOp& curop, const char * oplog_ns , OpTime lastOp );

    /** @return true if op has made it to w servers */
    bool opReplicatedEnough( OpTime op , int w );
    bool opReplicatedEnough( OpTime op , BSONElement w );

    void resetSlaveCache();
    unsigned getSlaveCount();
}
