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
#include "replutil.h"

namespace mongo {

    map<string,Command*> * Command::_commandsByBestName;
    map<string,Command*> * Command::_webCommands;
    map<string,Command*> * Command::_commands;

    string Command::parseNsFullyQualified(const string& dbname, const BSONObj& cmdObj) const { 
        string s = cmdObj.firstElement().valuestr();
        NamespaceString nss(s);
        // these are for security, do not remove:
        massert(15962, "need to specify namespace" , !nss.db.empty() );
        massert(15966, str::stream() << "dbname not ok in Command::parseNsFullyQualified: " << dbname , dbname == nss.db || dbname == "admin" );
        return s;
    }

    /*virtual*/ string Command::parseNs(const string& dbname, const BSONObj& cmdObj) const {
        string coll = cmdObj.firstElement().valuestr();
#if defined(CLC)
        DEV if( mongoutils::str::startsWith(coll, dbname+'.') ) { 
            log() << "DEBUG parseNs Command's collection name looks like it includes the db name\n"
                << dbname << '\n' 
                << coll << '\n'
                << cmdObj.toString() << endl;
            dassert(false);
        }
#endif
        return dbname + '.' + coll;
    }

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
        if( lockGlobally() ) 
            ss << " lockGlobally ";
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

    Command* Command::findCommand( const string& name ) {
        map<string,Command*>::iterator i = _commands->find( name );
        if ( i == _commands->end() )
            return 0;
        return i->second;
    }


    Command::LockType Command::locktype( const string& name ) {
        Command * c = findCommand( name );
        if ( ! c )
            return WRITE;
        return c->locktype();
    }

    void Command::logIfSlow( const Timer& timer, const string& msg ) {
        int ms = timer.millis();
        if ( ms > cmdLine.slowMS ) {
            out() << msg << " took " << ms << " ms." << endl;
        }
    }

}

#include "../client/connpool.h"

namespace mongo {

    extern DBConnectionPool pool;

    class PoolFlushCmd : public Command {
    public:
        PoolFlushCmd() : Command( "connPoolSync" , false , "connpoolsync" ) {}
        virtual void help( stringstream &help ) const { help<<"internal"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool run(const string&, mongo::BSONObj&, int, std::string&, mongo::BSONObjBuilder& result, bool) {
            pool.flush();
            return true;
        }
        virtual bool slaveOk() const {
            return true;
        }

    } poolFlushCmd;

    class PoolStats : public Command {
    public:
        PoolStats() : Command( "connPoolStats" ) {}
        virtual void help( stringstream &help ) const { help<<"stats about connection pool"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool run(const string&, mongo::BSONObj&, int, std::string&, mongo::BSONObjBuilder& result, bool) {
            pool.appendInfo( result );
            result.append( "numDBClientConnection" , DBClientConnection::getNumConnections() );
            result.append( "numAScopedConnection" , AScopedConnection::getNumConnections() );
            return true;
        }
        virtual bool slaveOk() const {
            return true;
        }

    } poolStatsCmd;

} // namespace mongo
