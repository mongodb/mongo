/* commands.cpp
   db "commands" (sent via db.$cmd.findOne(...))
 */

/**
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

#include "stdafx.h"
#include "jsobj.h"
#include "commands.h"

namespace mongo {

    map<string,Command*> *commands;

    Command::Command(const char *_name) : name(_name) {
        // register ourself.
        if ( commands == 0 )
            commands = new map<string,Command*>;
        (*commands)[name] = this;
    }

    bool runCommandAgainstRegistered(const char *ns, BSONObj& jsobj, BSONObjBuilder& anObjBuilder) {
        const char *p = strchr(ns, '.');
        if ( !p ) return false;
        if ( strcmp(p, ".$cmd") != 0 ) return false;

        bool ok = false;
        bool valid = false;

        BSONElement e;
        e = jsobj.firstElement();

        map<string,Command*>::iterator i;

        if ( e.eoo() )
            ;
        /* check for properly registered command objects.  Note that all the commands below should be
           migrated over to the command object format.
           */
        else if ( (i = commands->find(e.fieldName())) != commands->end() ) {
            valid = true;
            string errmsg;
            Command *c = i->second;
            if ( c->adminOnly() && strncmp(ns, "admin", 5) != 0 ) {
                ok = false;
                errmsg = "access denied";
            }
            else {
                ok = c->run(ns, jsobj, errmsg, anObjBuilder, false);
            }
            if ( !ok ) {
                anObjBuilder.append("errmsg", errmsg);
                uassert_nothrow(errmsg.c_str());
            }
            return true;
        }

        return false;
    }

} // namespace mongo
