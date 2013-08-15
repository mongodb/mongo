/*
 *    Copyright (C) 2010 10gen Inc.
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

// Tool.h

#pragma once

#include <string>

#if defined(_WIN32)
#include <io.h>
#endif

#include "mongo/db/instance.h"
#include "mongo/db/matcher.h"
#include "mongo/tools/tool_options.h"
#include "mongo/util/options_parser/environment.h"

using std::string;

namespace mongo {

    extern moe::OptionSection options;
    extern moe::Environment _params;

    class Tool {
    public:
        Tool( string name , string defaultDB="test" ,
              string defaultCollection="", bool usesstdout=true, bool quiet=false);
        virtual ~Tool();

        static auto_ptr<Tool> (*createInstance)();

        int main( int argc , char ** argv, char ** envp );

        string getParam( string name , string def="" ) {
            if ( _params.count( name ) )
                return _params[name.c_str()].as<string>();
            return def;
        }
        int getParam( string name , int def ) {
            if ( _params.count( name ) )
                return _params[name.c_str()].as<int>();
            return def;
        }
        bool hasParam( string name ) {
            return _params.count( name );
        }

        string getNS() {
            if ( _coll.size() == 0 ) {
                cerr << "no collection specified!" << endl;
                throw -1;
            }
            return _db + "." + _coll;
        }

        string getAuthenticationDatabase();

        void useStandardOutput( bool mode ) {
            _usesstdout = mode;
        }

        bool isMaster();
        bool isMongos();
        
        virtual void preSetup() {}

        virtual int run() = 0;

        virtual void printHelp(ostream &out) = 0;

        virtual void printVersion(ostream &out);

    protected:

        mongo::DBClientBase &conn( bool slaveIfPaired = false );

        string _name;

        string _db;
        string _coll;
        string _fileName;

        string _username;
        string _password;
        string _authenticationDatabase;
        string _authenticationMechanism;

        bool _usesstdout;
        bool _quiet;
        bool _noconnection;
        bool _autoreconnect;

        void needFields();

        vector<string> _fields;
        BSONObj _fieldsObj;


        string _host;

    protected:

        mongo::DBClientBase * _conn;
        mongo::DBClientBase * _slaveConn;
        bool _paired;

    private:
        void auth();
    };

    class BSONTool : public Tool {
        bool _objcheck;
        auto_ptr<Matcher> _matcher;

    public:
        BSONTool( const char * name , bool objcheck = true );

        virtual int doRun() = 0;
        virtual void gotObject( const BSONObj& obj ) = 0;

        virtual int run();

        long long processFile( const boost::filesystem::path& file );

    };

}

#define REGISTER_MONGO_TOOL(TYPENAME) \
    auto_ptr<Tool> createInstanceOfThisTool() {return auto_ptr<Tool>(new TYPENAME());} \
    auto_ptr<Tool> (*Tool::createInstance)() = createInstanceOfThisTool;
