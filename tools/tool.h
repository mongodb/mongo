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

#include "client/dbclient.h"
#include "db/instance.h"

using std::string;

namespace mongo {

    class Tool {
    public:
        Tool( string name , bool localDBAllowed=true, string defaultDB="test" , string defaultCollection="");
        virtual ~Tool();

        int main( int argc , char ** argv );

        boost::program_options::options_description_easy_init add_options(){
            return _options->add_options();
        }
        boost::program_options::options_description_easy_init add_hidden_options(){
            return _hidden_options->add_options();
        }
        void addPositionArg( const char * name , int pos ){
            _positonalOptions.add( name , pos );
        }

        string getParam( string name , string def="" ){
            if ( _params.count( name ) )
                return _params[name.c_str()].as<string>();
            return def;
        }
        bool hasParam( string name ){
            return _params.count( name );
        }

        string getNS(){
            if ( _coll.size() == 0 ){
                cerr << "no collection specified!" << endl;
                throw -1;
            }
            return _db + "." + _coll;
        }

        virtual int run() = 0;

        virtual void printHelp(ostream &out);

        virtual void printExtraHelp( ostream & out );

    protected:

        mongo::DBClientBase &conn( bool slaveIfPaired = false );
        void auth( string db = "" );
        
        string _name;

        string _db;
        string _coll;

        string _username;
        string _password;

        void addFieldOptions();
        void needFields();
        
        vector<string> _fields;
        BSONObj _fieldsObj;

        
    private:
        string _host;
        mongo::DBClientBase * _conn;
        bool _paired;

        boost::program_options::options_description * _options;
        boost::program_options::options_description * _hidden_options;
        boost::program_options::positional_options_description _positonalOptions;

        boost::program_options::variables_map _params;

    };

}
