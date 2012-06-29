// authentication_table.cpp

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


#include "mongo/client/authentication_table.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthenticationTable internalSecurityAuthenticationTable;
    const string AuthenticationTable::fieldName = "$auth";

    const AuthenticationTable& AuthenticationTable::getInternalSecurityAuthenticationTable() {
        return internalSecurityAuthenticationTable;
    }

    AuthenticationTable& AuthenticationTable::getMutableInternalSecurityAuthenticationTable() {
        return internalSecurityAuthenticationTable;
    }

    void AuthenticationTable::addAuth(const std::string& dbname , const std::string& user,
                                      const Auth::Level& level ) {
        Auth auth;
        auth.level = level;
        auth.user = user;
        addAuth( dbname, auth );
    }

    void AuthenticationTable::addAuth(const std::string& dbname , const Auth& auth) {
        _dbs[dbname] = auth;
    }

    void AuthenticationTable::removeAuth(const std::string& dbname ) {
        _dbs.erase(dbname);
    }

    void AuthenticationTable::clearAuth() {
        _dbs.clear();
    }

    Auth AuthenticationTable::getAuthForDb( const std::string& dbname ) const {
        return mapFindWithDefault( _dbs, dbname, Auth() );
    }

    // Takes the authentication state from the given BSONObj
    void AuthenticationTable::setFromBSON( const BSONObj& obj ) {
        _dbs.clear();

        BSONObjIterator it( obj );
        while ( it.more() ) {
            BSONElement dbInfo = it.next();
            BSONElement subObj = dbInfo.Obj().firstElement();
            Auth auth;
            auth.user = subObj.fieldName();
            auth.level = static_cast<Auth::Level>(subObj.Int());
            _dbs[dbInfo.fieldName()] = auth;
        }
    }

    BSONObj AuthenticationTable::toBSON() const {
        BSONObjBuilder b;
        for ( DBAuthMap::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i ) {
            BSONObjBuilder temp( b.subobjStart( i->first ) );
            temp.append( i->second.user, i->second.level );
            temp.done();
        }
        return b.obj();
    }

    BSONObj AuthenticationTable::copyCommandObjAddingAuth( const BSONObj& cmdObj ) const {
        BSONObjBuilder cmdWithAuth;

        // Copy all elements of the original command object, but don't take user-provided $auth.
        BSONObjIterator it(cmdObj);
        while ( it.more() ) {
            BSONElement e = it.next();
            if ( mongoutils::str::equals( e.fieldName(), fieldName.c_str() ) ) continue;
            cmdWithAuth.append(e);
        }

        cmdWithAuth.append( fieldName, toBSON() );
        return cmdWithAuth.obj();
    }

}
