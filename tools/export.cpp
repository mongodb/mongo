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

#include "Tool.h"

#include <fstream>
#include <iostream>

#include <boost/program_options.hpp>
#include <pcrecpp.h>

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
        if ( hasParam( "out" ) )
            outPtr = new ofstream( outfile.c_str() );
        ostream &out = *outPtr;

        BSONObj * fieldsToReturn = 0;
        BSONObj realFieldsToReturn;

        vector<string> fields;

        try {
            ns = getNS();
        } catch (...) {
            printHelp(cerr);
            return 1;
        }

        auth();

        if ( hasParam( "fields" ) ){

            BSONObjBuilder b;

            string fields_arg = getParam("fields");
            pcrecpp::StringPiece input(fields_arg);

            string f;
            pcrecpp::RE re("([\\w\\.]+),?" );
            while ( re.Consume( &input, &f ) ){
                fields.push_back( f );
                b.append( f.c_str() , 1 );
            }

            realFieldsToReturn = b.obj();
            fieldsToReturn = &realFieldsToReturn;
        }

        if ( csv && fields.size() == 0 ){
            cerr << "csv mode requires a field list" << endl;
            return -1;
        }


        auto_ptr<DBClientCursor> cursor = conn().query( ns.c_str() , ((Query)(getParam( "query" , "" ))).snapshot() , 0 , 0 , fieldsToReturn , Option_SlaveOk | Option_NoCursorTimeout );

        if ( csv ){
            for ( vector<string>::iterator i=fields.begin(); i != fields.end(); i++ ){
                if ( i != fields.begin() )
                    out << ",";
                out << *i;
            }
            out << endl;
        }

        while ( cursor->more() ) {
            BSONObj obj = cursor->next();
            if ( csv ){
                for ( vector<string>::iterator i=fields.begin(); i != fields.end(); i++ ){
                    if ( i != fields.begin() )
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


        return 0;
    }
};

int main( int argc , char ** argv ) {
    Export e;
    return e.main( argc , argv );
}
