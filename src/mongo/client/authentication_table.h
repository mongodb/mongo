// authentication_table.h

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <string>

#include "mongo/client/authlevel.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * An AuthenticationTable object is present within every AuthenticationInfo object and
     * contains the map of dbname to auth level for the current client.
     * All syncronization to this class is done through its encompassing AuthenticationInfo.
     */
    class AuthenticationTable {
    public:
        AuthenticationTable() {}
        ~AuthenticationTable() {}

        void addAuth( const std::string& dbname,
                      const std::string& user,
                      const Auth::Level& level );

        void addAuth( const std::string& dbname , const Auth& auth );

        void removeAuth( const std::string& dbname );

        void clearAuth();

        Auth getAuthForDb( const std::string& dbname ) const;

        // Takes the authentication state from the given BSONObj, replcacing whatever state it had.
        void setFromBSON( const BSONObj& obj );

        BSONObj toBSON() const;

    private:
        typedef map<std::string,Auth> DBAuthMap;
        DBAuthMap _dbs; // dbname -> auth
    };
}
