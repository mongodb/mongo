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

#include "pch.h"
#include "jsobj.h"
#include "commands.h"
#include "client.h"
#include "replpair.h"

namespace mongo {

    map<string,Command*> * Command::_commandsByBestName;
    map<string,Command*> * Command::_webCommands;
    map<string,Command*> * Command::_commands;

    void Command::htmlHelp(stringstream& ss) const {
        string helpStr;
        {
            stringstream h;
            help(h);
            helpStr = h.str();
        }
        ss << "\n<tr><td>";
        bool web = _webCommands->count(name) != 0;
        if( web ) ss << "<a href=\"/" << name << "?text=1\">";
        ss << name;
        if( web ) ss << "</a>";
        ss << "</td>\n";
        ss << "<td>";
        int l = locktype();
        //if( l == NONE ) ss << "N ";
        if( l == READ ) ss << "R ";
        else if( l == WRITE ) ss << "W ";
        if( slaveOk() )
            ss << "S ";
        if( adminOnly() )
            ss << "A";
        ss << "</td>";
        ss << "<td>";
        if( helpStr != "no help defined" ) {
            const char *p = helpStr.c_str();
            while( *p ) { 
                if( *p == '<' ) {
                    ss << "&lt;";
                    p++; continue;
                }
                else if( *p == '{' )
                    ss << "<code>";
                else if( *p == '}' ) {
                    ss << "}</code>";
                    p++;
                    continue;
                }
                if( strncmp(p, "http:", 5) == 0 ) { 
                    ss << "<a href=\"";
                    const char *q = p;
                    while( *q && *q != ' ' && *q != '\n' )
                        ss << *q++;
                    ss << "\">";
                    q = p;
                    if( startsWith(q, "http://www.mongodb.org/display/") )
                        q += 31;
                    while( *q && *q != ' ' && *q != '\n' ) {
                        ss << (*q == '+' ? ' ' : *q);
                        q++;
                        if( *q == '#' ) 
                            while( *q && *q != ' ' && *q != '\n' ) q++;
                    }
                    ss << "</a>";
                    p = q;
                    continue;
                }
                if( *p == '\n' ) ss << "<br>";
                else ss << *p;
                p++;
            }
        }
        ss << "</td>";
        ss << "</tr>\n";
    }

    Command::Command(const char *_name, bool web, const char *oldName) : name(_name) {
        // register ourself.
        if ( _commands == 0 )
            _commands = new map<string,Command*>;
        if( _commandsByBestName == 0 )
            _commandsByBestName = new map<string,Command*>;
        Command*& c = (*_commands)[name];
        if ( c )
            log() << "warning: 2 commands with name: " << _name << endl;
        c = this;
        (*_commandsByBestName)[name] = this;

        if( web ) {
            if( _webCommands == 0 )
                _webCommands = new map<string,Command*>;
            (*_webCommands)[name] = this;
        }

        if( oldName )
            (*_commands)[oldName] = this;
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

        BSONElement e = jsobj.firstElement();
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
            if ( c->adminOnly() && !startsWith(ns, "admin.") ) {
                ok = false;
                errmsg = "access denied - use admin db";
            }
            else if ( jsobj.getBoolField( "help" ) ){
                stringstream help;
                help << "help for: " << e.fieldName() << " ";
                c->help( help );
                anObjBuilder.append( "help" , help.str() );
            }
            else {
                ok = c->run( nsToDatabase( ns ) , jsobj, errmsg, anObjBuilder, false);
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

    void Command::logIfSlow( const Timer& timer, const string& msg ) {
        int ms = timer.millis();
        if ( ms > cmdLine.slowMS ){
            out() << msg << " took " << ms << " ms." << endl;
        }
    }
    
    
} // namespace mongo
