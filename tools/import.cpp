// import.cpp

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

class Import : public Tool {
    
    enum Type { JSON , CSV , TSV };
    Type _type;

    const char * _sep;
    bool _ignoreBlanks;
    bool _headerLine;
    
    void _append( BSONObjBuilder& b , const string& fieldName , const string& data ){
        if ( b.appendAsNumber( fieldName , data ) )
            return;
        
        if ( _ignoreBlanks && data.size() == 0 )
            return;

        // TODO: other types?
        b.append( fieldName.c_str() , data );
    }
    
    BSONObj parseLine( char * line ){
        if ( _type == JSON ){
            char * end = ( line + strlen( line ) ) - 1;
            while ( isspace(*end) ){
                *end = 0;
                end--;
            }
            return fromjson( line );
        }
        
        BSONObjBuilder b;

        unsigned int pos=0;
        while ( line[0] ){
            string name;
            if ( pos < _fields.size() ){
                name = _fields[pos];
            }
            else {
                stringstream ss;
                ss << "field" << pos;
                name = ss.str();
            }
            pos++;
            
            int skip = 1;
            char * end;
            if ( _type == CSV && line[0] == '"' ){
                line++;
                end = strstr( line , "\"" );
                skip = 2;
            }
            else {
                end = strstr( line , _sep );
            }
            
            bool done = false;
            string data;

            if ( ! end ){
                done = true;
                data = string( line );
            }
            else {
                data = string( line , end - line );
            }
            
            if ( _headerLine )
                _fields.push_back( data );
            else
                _append( b , name , data );
            
            if ( done )
                break;
            line = end + skip;
        }
        return b.obj();
    }
    
public:
    Import() : Tool( "import" ){
        addFieldOptions();
        add_options()
            ("ignoreBlanks","if given, empty fields in csv and tsv will be ignored")
            ("type",po::value<string>() , "type of file to import.  default: json (json,csv,tsv)")
            ("file",po::value<string>() , "file to import from; if not specified stdin is used" )
            ("drop", "drop collection first " )
            ("headerline","CSV,TSV only - use first line as headers")
            ;
        addPositionArg( "file" , 1 );
        _type = JSON;
        _ignoreBlanks = false;
        _headerLine = false;
    }
    
    int run(){
        string filename = getParam( "file" );
        long long fileSize = -1;

        istream * in = &cin;

        ifstream file( filename.c_str() , ios_base::in | ios_base::binary);

        if ( filename.size() > 0 && filename != "-" ){
            if ( ! exists( filename ) ){
                cerr << "file doesn't exist: " << filename << endl;
                return -1;
            }
            in = &file;
            fileSize = file_size( filename );
        }

        string ns;

        try {
            ns = getNS();
        } catch (...) {
            printHelp(cerr);
            return -1;
        }
        
        log(1) << "ns: " << ns << endl;
        
        auth();

        if ( hasParam( "drop" ) ){
            cout << "dropping: " << ns << endl;
            conn().dropCollection( ns.c_str() );
        }

        if ( hasParam( "ignoreBlanks" ) ){
            _ignoreBlanks = true;
        }

        if ( hasParam( "type" ) ){
            string type = getParam( "type" );
            if ( type == "json" )
                _type = JSON;
            else if ( type == "csv" ){
                _type = CSV;
                _sep = ",";
            }
            else if ( type == "tsv" ){
                _type = TSV;
                _sep = "\t";
            }
            else {
                cerr << "don't know what type [" << type << "] is" << endl;
                return -1;
            }
        }
        
        if ( _type == CSV || _type == TSV ){
            _headerLine = hasParam( "headerline" );
            if ( ! _headerLine )
                needFields();
        }

        int errors = 0;
        
        int num = 0;
        
        time_t start = time(0);

        log(1) << "filesize: " << fileSize << endl;
        ProgressMeter pm( fileSize );
        const int BUF_SIZE = 1024 * 1024 * 4;
        boost::scoped_array<char> line (new char[BUF_SIZE]);
        while ( *in ){
            char * buf = line.get();

            in->getline( buf , BUF_SIZE );
            uassert( "unknown error reading file" , ( in->rdstate() & ios_base::badbit ) == 0 );
            log(1) << "got line:" << buf << endl;

            while( isspace( buf[0] ) ) buf++;
            
            int len = strlen( buf );
            if ( ! len )
                continue;
            
            if ( in->rdstate() == ios_base::eofbit )
                break;
            assert( in->rdstate() == 0 );

            try {
                BSONObj o = parseLine( buf );
                if ( _headerLine )
                    _headerLine = false;
                else
                    conn().insert( ns.c_str() , o );
            }
            catch ( std::exception& e ){
                cout << "exception:" << e.what() << endl;
                cout << buf << endl;
                errors++;
            }

            num++;
            if ( pm.hit( len + 1 ) ){
                cout << "\t\t\t" << num << "\t" << ( num / ( time(0) - start ) ) << "/second" << endl;
            }
        }

        cout << "imported " << num << " objects" << endl;
        
        if ( errors == 0 )
            return 0;
        
        cerr << "encountered " << errors << " error" << ( errors == 1 ? "" : "s" ) << endl;
        return -1;
    }
};

int main( int argc , char ** argv ) {
    Import import;
    return import.main( argc , argv );
}
