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

#include <boost/program_options.hpp>

#if defined(_WIN32)
#include <io.h>
#endif

#include "db/instance.h"
#include "db/matcher.h"
#include "db/security.h"

using std::string;

namespace mongo {

    class Tool {
    public:
        enum DBAccess {
            NONE = 0 ,
            REMOTE_SERVER = 1 << 1 ,
            LOCAL_SERVER = 1 << 2 ,
            SPECIFY_DBCOL = 1 << 3 ,
            ALL = REMOTE_SERVER | LOCAL_SERVER | SPECIFY_DBCOL
        };

        Tool( string name , DBAccess access=ALL, string defaultDB="test" ,
              string defaultCollection="", bool usesstdout=true);
        virtual ~Tool();

        int main( int argc , char ** argv );

        boost::program_options::options_description_easy_init add_options() {
            return _options->add_options();
        }
        boost::program_options::options_description_easy_init add_hidden_options() {
            return _hidden_options->add_options();
        }
        void addPositionArg( const char * name , int pos ) {
            _positonalOptions.add( name , pos );
        }

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

        void useStandardOutput( bool mode ) {
            _usesstdout = mode;
        }

        bool isMaster();
        bool isMongos();
        
        virtual void preSetup() {}

        virtual int run() = 0;

        virtual void printHelp(ostream &out);

        virtual void printExtraHelp( ostream & out ) {}
        virtual void printExtraHelpAfter( ostream & out ) {}

        virtual void printVersion(ostream &out);

    protected:

        mongo::DBClientBase &conn( bool slaveIfPaired = false );
        void auth( string db = "",  Auth::Level * level = NULL);

        string _name;

        string _db;
        string _coll;
        string _fileName;

        string _username;
        string _password;

        bool _usesstdout;
        bool _noconnection;
        bool _autoreconnect;

        void addFieldOptions();
        void needFields();

        vector<string> _fields;
        BSONObj _fieldsObj;


        string _host;

    protected:

        mongo::DBClientBase * _conn;
        mongo::DBClientBase * _slaveConn;
        bool _paired;

        boost::program_options::options_description * _options;
        boost::program_options::options_description * _hidden_options;
        boost::program_options::positional_options_description _positonalOptions;

        boost::program_options::variables_map _params;

    };

    class BSONTool : public Tool {
        bool _objcheck;
        auto_ptr<Matcher> _matcher;

    public:
        BSONTool( const char * name , DBAccess access=ALL, bool objcheck = false );

        virtual int doRun() = 0;
        virtual void gotObject( const BSONObj& obj ) = 0;

        virtual int run();

        long long processFile( const boost::filesystem::path& file );

    };

}
