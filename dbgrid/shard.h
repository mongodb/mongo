/* shard.h

   A "shard" is a database (replica pair typically) which represents
   one partition of the overall database.
*/

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

#include "../client/dbclient.h"
#include "../client/model.h"

namespace mongo {

/* grid.shards
     { name: 'hostname'
     }
*/
class Shard : public Model {
public:
    string name; // hostname (less -l, -r)

    virtual const char * getNS() {
        return "grid.shards";
    }
    virtual void serialize(BSONObjBuilder& to);
    virtual void unserialize(BSONObj& from);
    virtual DBClientWithCommands* conn();
};

} // namespace mongo
