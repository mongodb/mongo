// d_logic.h
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


#pragma once

#include "../pch.h"

namespace mongo {

    /**
     * @return true if we have any shard info for the ns
     */
    bool haveLocalShardingInfo( const string& ns );
    
    /**
     * @return true if the current threads shard version is ok, or not in sharded version
     */
    bool shardVersionOk( const string& ns , string& errmsg );

    /**
     * @return true if we took care of the message and nothing else should be done
     */
    bool handlePossibleShardedMessage( Message &m, DbResponse &dbresponse );
}
