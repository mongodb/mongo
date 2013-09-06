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
