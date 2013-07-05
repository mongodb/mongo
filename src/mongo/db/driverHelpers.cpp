// driverHelpers.cpp

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

/**
   this file has dbcommands that are for drivers
   mostly helpers
*/


#include "mongo/pch.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/background.h"

namespace mongo {

    class BasicDriverHelper : public Command {
    public:
        BasicDriverHelper( const char * name ) : Command( name ) {}

        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool slaveOverrideOk() const { return true; }
    };

    class ObjectIdTest : public BasicDriverHelper {
    public:
        ObjectIdTest() : BasicDriverHelper( "driverOIDTest" ) {}
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if ( cmdObj.firstElement().type() != jstOID ) {
                errmsg = "not oid";
                return false;
            }

            const OID& oid = cmdObj.firstElement().__oid();
            result.append( "oid" , oid );
            result.append( "str" , oid.str() );

            return true;
        }
    } driverObjectIdTest;
}
