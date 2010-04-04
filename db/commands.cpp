/* commands.cpp
   db "commands" (sent via db.$cmd.findOne(...))
 */

/*    Copyright 2009 10gen Inc.
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

#include "stdafx.h"
#include "jsobj.h"
#include "commands.h"
#include "client.h"
#include "replset.h"

namespace mongo {

    map<string,Command*> * Command::_commands;

    Command::Command(const char *_name) : name(_name) {
        // register ourself.
        if ( _commands == 0 )
            _commands = new map<string,Command*>;
        Command*& c = (*_commands)[name];
        if ( c )
            log() << "warning: 2 commands with name: " << _name << endl;
        c = this;
    }

    void Command::help( stringstream& help ) const {
        help << "no help defined";
    }
    
    bool Command::runAgainstRegistered(const char *ns, BSONObj& jsobj, BSONObjBuilder& anObjBuilder) {
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
        else if ( (i = _commands->find(e.fieldName())) != _commands->end() ) {
            valid = true;
            string errmsg;
            Command *c = i->second;
            if ( c->adminOnly() && strncmp(ns, "admin", 5) != 0 ) {
                ok = false;
                errmsg = "access denied";
            }
            else if ( jsobj.getBoolField( "help" ) ){
                stringstream help;
                help << "help for: " << e.fieldName() << " ";
                c->help( help );
                anObjBuilder.append( "help" , help.str() );
            }
            else {
                ok = c->run(ns, jsobj, errmsg, anObjBuilder, false);
            }

            BSONObj tmp = anObjBuilder.asTempObj();
            bool have_ok = tmp.hasField("ok");
            bool have_errmsg = tmp.hasField("errmsg");

            if (!have_ok)
                anObjBuilder.append( "ok" , ok ? 1.0 : 0.0 );
            
            if ( !ok && !have_errmsg) {
                anObjBuilder.append("errmsg", errmsg);
                uassert_nothrow(errmsg.c_str());
            }
            return true;
        }
        
        return false;
    }

    Command* Command::findCommand( const string& name ){
        map<string,Command*>::iterator i = _commands->find( name );
        if ( i == _commands->end() )
            return 0;
        return i->second;
    }


    Command::LockType Command::locktype( const string& name ){
        Command * c = findCommand( name );
        if ( ! c )
            return WRITE;
        return c->locktype();
    }

    
} // namespace mongo
