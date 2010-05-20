// util.h

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
#include "../client/dbclient.h"

/**
   some generic sharding utils that can be used in mongod or mongos
 */

namespace mongo {
    
    /** 
     * your config info for a given shard/chunk is out of date 
     */
    class StaleConfigException : public AssertionException {
    public:
        StaleConfigException( const string& ns , const string& raw){
            code = 9996;
            stringstream s;
            s << "StaleConfigException ns: " << ns << " " << raw;
            msg = s.str();
            log(1) << msg << endl;
        }
        
        virtual ~StaleConfigException() throw(){}

    private:
    };

    void checkShardVersion( DBClientBase & conn , const string& ns , bool authoritative = false );

}
