// export.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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
#include "client/dbclient.h"
#include "db/json.h"

#include "tool.h"

#include <fstream>
#include <iostream>

#include <boost/program_options.hpp>

using namespace mongo;

namespace po = boost::program_options;

class Export : public Tool {
public:
    Export() : Tool( "export" ){
        add_options()
            ("query,q" , po::value<string>() , "query filter, as a JSON string" )
            ("fields,f" , po::value<string>() , "comma seperated list of field names e.g. -f name,age" )
            ("csv","export to csv instead of json")
            ("out,o", po::value<string>(), "output file; if not specified, stdout is used")
            ;
    }
    
    int run(){
        string ns;
        const bool csv = hasParam( "csv" );
        ostream *outPtr = &cout;
        string outfile = getParam( "out" );
        auto_ptr<ofstream> fileStream;
        if ( hasParam( "out" ) ){
            size_t idx = outfile.rfind( "/" );
            if ( idx != string::npos ){
                string dir = outfile.substr( 0 , idx + 1 );
                create_directories( dir );
            }
            ofstream * s = new ofstream( outfile.c_str() , ios_base::out | ios_base::binary );
            fileStream.reset( s );
            outPtr = s;
            if ( ! s->good() ){
                cerr << "couldn't open [" << outfile << "]" << endl;
                return -1;
            }
        }
        ostream &out = *outPtr;

        BSONObj * fieldsToReturn = 0;
        BSONObj realFieldsToReturn;

        try {
            ns = getNS();
        } catch (...) {
            printHelp(cerr);
            return 1;
        }

        auth();

        if ( hasParam( "fields" ) ){
            needFields();
            fieldsToReturn = &_fieldsObj;
        }


        if ( csv && _fields.size() == 0 ){
            cerr << "csv mode requires a field list" << endl;
            return -1;
        }


        auto_ptr<DBClientCursor> cursor = conn().query( ns.c_str() , ((Query)(getParam( "query" , "" ))).snapshot() , 0 , 0 , fieldsToReturn , Option_SlaveOk | Option_NoCursorTimeout );

        if ( csv ){
            for ( vector<string>::iterator i=_fields.begin(); i != _fields.end(); i++ ){
                if ( i != _fields.begin() )
                    out << ",";
                out << *i;
            }
            out << endl;
        }
        
        long long num = 0;
        while ( cursor->more() ) {
            num++;
            BSONObj obj = cursor->next();
            if ( csv ){
                for ( vector<string>::iterator i=_fields.begin(); i != _fields.end(); i++ ){
                    if ( i != _fields.begin() )
                        out << ",";
                    const BSONElement & e = obj.getFieldDotted(i->c_str());
                    if ( ! e.eoo() ){
                        out << e.jsonString( Strict , false );
                    }
                }
                out << endl;
            }
            else {
                out << obj.jsonString() << endl;
            }
        }

        
        cout << "exported " << num << " records" << endl;

        return 0;
    }
};

int main( int argc , char ** argv ) {
    Export e;
    return e.main( argc , argv );
}
