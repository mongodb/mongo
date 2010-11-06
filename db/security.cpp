// security.cpp

/**
 *    Copyright (C) 2009 10gen Inc.
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
#include "security.h"
#include "instance.h"
#include "client.h"
#include "curop-inl.h"
#include "db.h"
#include "dbhelpers.h"

namespace mongo {

    bool noauth = true;
    
	int AuthenticationInfo::warned = 0;

    void AuthenticationInfo::print(){
        cout << "AuthenticationInfo: " << this << '\n';
        for ( map<string,Auth>::iterator i=m.begin(); i!=m.end(); i++ ){
            cout << "\t" << i->first << "\t" << i->second.level << '\n';
        }
        cout << "END" << endl;
    }


    bool AuthenticationInfo::_isAuthorizedSpecialChecks( const string& dbname ) {
        if ( cc().isGod() ){
            return true;
        }
        
        if ( isLocalHost ){
            atleastreadlock l(""); 
            Client::GodScope gs;
            Client::Context c("admin.system.users");
            BSONObj result;
            if( ! Helpers::getSingleton("admin.system.users", result) ){
                if( warned == 0 ) {
                    warned++;
                    log() << "note: no users configured in admin.system.users, allowing localhost access" << endl;
                }
                return true;
            }
        }
        return false;
    }

} // namespace mongo

