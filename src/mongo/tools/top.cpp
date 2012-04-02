// top.cpp

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

#include "pch.h"

#include "db/json.h"
#include "../util/text.h"
#include "tool.h"
#include <fstream>
#include <iostream>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace mongo {

    class TopTool : public Tool {
    public:

        TopTool() : Tool( "top" , REMOTE_SERVER , "admin" ) {
            _sleep = 1;

            add_hidden_options()
            ( "sleep" , po::value<int>() , "time to sleep between calls" )
            ;
            addPositionArg( "sleep" , 1 );

            _autoreconnect = true;
        }

        virtual void printExtraHelp( ostream & out ) {
            out << "View live MongoDB collection statistics.\n" << endl;
        }
        
        BSONObj getData() {
            BSONObj out;
            if ( ! conn().simpleCommand( _db , &out , "top" ) ) {
                cout << "error: " << out << endl;
                return BSONObj();
            }
            return out.getOwned();
        }
        
        void printDiff( BSONObj prev , BSONObj now ) { 
            if ( ! prev["totals"].isABSONObj() ||
                 ! now["totals"].isABSONObj() ) {
                cout << "." << endl;
                return;
            }

            prev = prev["totals"].Obj();
            now = now["totals"].Obj();
            
            vector<NSInfo> data;
            
            unsigned longest = 30;

            BSONObjIterator i( now );
            while ( i.more() ) {
                BSONElement e = i.next();
                
                // invalid, data fixed in 1.8.0
                if ( e.fieldName()[0] == '?' )
                    continue;
                
                if ( ! str::contains( e.fieldName() , '.' ) )
                    continue;
                
                BSONElement old = prev[e.fieldName()];
                if ( old.eoo() ) 
                    continue;
                
                if ( strlen( e.fieldName() ) > longest )
                    longest = strlen(e.fieldName());

                data.push_back( NSInfo( e.fieldName() , old.Obj() , e.Obj() ) );
            }
            
            std::sort( data.begin() , data.end() );

            cout << "\n"
                 << setw(longest) << "ns" 
                 << "\ttotal"  
                 << "\tread"    
                 << "\twrite"  
                 << "\t\t" << terseCurrentTime()
                 << endl;
            for ( int i=data.size()-1; i>=0 && data.size() - i < 10 ; i-- ) {
                cout << setw(longest) << data[i].ns 
                     << "\t" << setprecision(3) << data[i].diffTimeMS( "total" ) << "ms" 
                     << "\t" << setprecision(3) << data[i].diffTimeMS( "readLock" ) << "ms" 
                     << "\t" << setprecision(3) << data[i].diffTimeMS( "writeLock" ) << "ms" 
                     << endl;
            }
        }

        int run() {
            _sleep = getParam( "sleep" , _sleep );

            auth();
            
            BSONObj prev = getData();

            while ( true ) {
                sleepsecs( _sleep );
                
                BSONObj now;
                try {
                    now = getData();
                }
                catch ( std::exception& e ) {
                    cout << "can't get data: " << e.what() << endl;
                    continue;
                }

                if ( now.isEmpty() )
                    return -2;
                
                try {
                    printDiff( prev , now );
                }
                catch ( AssertionException& e ) {
                    cout << "\nerror: " << e.what() << "\n"
                         << now
                         << endl;
                }


                prev = now;
            }

            return 0;
        }

        struct NSInfo {
            NSInfo( string thens , BSONObj a , BSONObj b ) {
                ns = thens;
                prev = a;
                cur = b;
                
                timeDiff = diffTime( "total" );
            }
            
            
            int diffTimeMS( const char * field  ) const {
                return (int)(diffTime( field ) / 1000);
            }

            double diffTime( const char * field ) const {
                return diff( field , "time" );
            }
            
            double diffCount( const char * field ) const {
                return diff( field , "count" );
            }

            /**
             * @param field total,readLock, etc...
             * @param type time or count
             */
            double diff( const char * field , const char * type ) const {
                return cur[field].Obj()[type].number() - prev[field].Obj()[type].number();
            }

            bool operator<(const NSInfo& r) const {
                return timeDiff < r.timeDiff;
            }
            
            string ns;
            
            BSONObj prev;
            BSONObj cur;

            double timeDiff; // time diff between prev and cur
        };

    private:
        int _sleep;
    };

}

int main( int argc , char ** argv ) {
    mongo::TopTool top;
    return top.main( argc , argv );
}

