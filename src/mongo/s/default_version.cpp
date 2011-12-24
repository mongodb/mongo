// @file default_version.cpp

/**
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

#include "pch.h"
#include "s/util.h"
#include "shard_version.h"

namespace mongo {

    // Global version manager
    VersionManager versionManager;

    void VersionManager::resetShardVersionCB( DBClientBase * conn ) {
        return;
    }

    bool VersionManager::isVersionableCB( DBClientBase* conn ){
        return false;
    }

    bool VersionManager::initShardVersionCB( DBClientBase * conn_in, BSONObj& result ){
        return false;
    }

    bool VersionManager::forceRemoteCheckShardVersionCB( const string& ns ){
        return true;
    }

    bool VersionManager::checkShardVersionCB( DBClientBase* conn_in , const string& ns , bool authoritative , int tryNumber ) {
        return false;
    }

    bool VersionManager::checkShardVersionCB( ShardConnection* conn_in , bool authoritative , int tryNumber ) {
        return false;
    }

}  // namespace mongo
