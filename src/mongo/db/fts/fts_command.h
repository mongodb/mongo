// fts_command.h

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

#pragma once

#include <string>
#include <vector>

#include "mongo/db/commands.h"

namespace mongo {

    namespace fts {

        class FTSCommand : public Command {
        public:
            FTSCommand();

            bool slaveOk() const { return true; }
            bool slaveOverrideOk() const { return true; }

            LockType locktype() const;

            void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out);


            bool run(const string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result,
                     bool fromRepl);

        protected:
            bool _run( const string& dbName,
                       BSONObj& cmdObj,
                       int cmdOptions,
                       const string& ns,
                       const string& searchString,
                       string language, // "" for not-set
                       int limit,
                       BSONObj& filter,
                       BSONObj& projection,
                       string& errmsg,
                       BSONObjBuilder& result );
        };

    }

}

