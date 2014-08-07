// dbhash.h

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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/db/commands.h"

namespace mongo {

    void logOpForDbHash( const char* ns );

    class DBHashCmd : public Command {
    public:
        DBHashCmd();

        virtual bool slaveOk() const { return true; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out);

        virtual bool run(OperationContext* txn, const std::string& dbname , BSONObj& cmdObj, int, std::string& errmsg, BSONObjBuilder& result, bool);

        void wipeCacheForCollection( const StringData& ns );

    private:

        bool isCachable( const StringData& ns ) const;

        std::string hashCollection( OperationContext* opCtx, Database* db, const std::string& fullCollectionName, bool* fromCache );

        std::map<std::string,std::string> _cachedHashed;
        mutex _cachedHashedMutex;

    };

}
