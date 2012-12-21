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

#include "mongo/db/jsobj.h"

/**
   some generic sharding utils that can be used in mongod or mongos
 */

namespace mongo {

    class ShardConnection;
    class DBClientBase;

    class VersionManager {
    public:
        VersionManager(){};

        bool isVersionableCB( DBClientBase* );
        bool initShardVersionCB( DBClientBase*, BSONObj& );
        bool forceRemoteCheckShardVersionCB( const string& );
        bool checkShardVersionCB( DBClientBase*, const string&, bool, int );
        bool checkShardVersionCB( ShardConnection*, bool, int );
        void resetShardVersionCB( DBClientBase* );

    };

    extern VersionManager versionManager;

} // namespace mongo
