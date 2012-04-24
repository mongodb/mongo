/** @file resetapi.cpp
    web rest api
*/
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
#include "../util/net/miniwebserver.h"
#include "../util/mongoutils/html.h"
#include "../util/md5.hpp"
#include "instance.h"
#include "dbwebserver.h"
#include "dbhelpers.h"
#include "repl.h"
#include "replutil.h"
#include "clientcursor.h"
#include "background.h"

#include "restapi.h"

namespace mongo {

    extern const char *replInfo;
    bool getInitialSyncCompleted();

    using namespace bson;
    using namespace mongoutils::html;

    class RESTHandler : public DbWebHandler {
    public:
        RESTHandler() : DbWebHandler( "DUMMY REST" , 1000 , true ) {}

        virtual bool handles( const string& url ) const {
            return
                url[0] == '/' &&
                url.find_last_of( '/' ) > 0;
        }

        virtual void handle( const char *rq, string url, BSONObj params,
                             string& responseMsg, int& responseCode,
                             vector<string>& headers,  const SockAddr &from ) {

            string::size_type first = url.find( "/" , 1 );
            if ( first == string::npos ) {
                responseCode = 400;
                return;
            }

            string method = MiniWebServer::parseMethod( rq );
            string dbname = url.substr( 1 , first - 1 );
            string coll = url.substr( first + 1 );
            string action = "";

            string::size_type last = coll.find_last_of( "/" );
            if ( last == string::npos ) {
                action = coll;
                coll = "_defaultCollection";
            }
            else {
                action = coll.substr( last + 1 );
                coll = coll.substr( 0 , last );
            }

            for ( string::size_type i=0; i<coll.size(); i++ )
                if ( coll[i] == '/' )
                    coll[i] = '.';

            string fullns = MiniWebServer::urlDecode(dbname + "." + coll);

            headers.push_back( (string)"x-action: " + action );
            headers.push_back( (string)"x-ns: " + fullns );

            bool html = false;

            stringstream ss;

            if ( method == "GET" ) {
                responseCode = 200;
                html = handleRESTQuery( fullns , action , params , responseCode , ss  );
            }
            else if ( method == "POST" ) {
                responseCode = 201;
                handlePost( fullns , MiniWebServer::body( rq ) , params , responseCode , ss  );
            }
            else {
                responseCode = 400;
                headers.push_back( "X_err: bad request" );
                ss << "don't know how to handle a [" << method << "]";
                out() << "don't know how to handle a [" << method << "]" << endl;
            }

            if( html )
                headers.push_back("Content-Type: text/html;charset=utf-8");
            else
                headers.push_back("Content-Type: text/plain;charset=utf-8");

            responseMsg = ss.str();
        }

        bool handleRESTQuery( string ns , string action , BSONObj & params , int & responseCode , stringstream & out ) {
            Timer t;

            int html = _getOption( params["html"] , 0 );
            int skip = _getOption( params["skip"] , 0 );
            int num  = _getOption( params["limit"] , _getOption( params["count" ] , 1000 ) ); // count is old, limit is new

            int one = 0;
            if ( params["one"].type() == String && tolower( params["one"].valuestr()[0] ) == 't' ) {
                num = 1;
                one = 1;
            }

            BSONObjBuilder queryBuilder;

            BSONObjIterator i(params);
            while ( i.more() ) {
                BSONElement e = i.next();
                string name = e.fieldName();
                if ( ! name.find( "filter_" ) == 0 )
                    continue;

                string field = name.substr(7);
                const char * val = e.valuestr();

                char * temp;

                // TODO: this is how i guess if something is a number.  pretty lame right now
                double number = strtod( val , &temp );
                if ( temp != val )
                    queryBuilder.append( field , number );
                else
                    queryBuilder.append( field , val );
            }

            BSONObj query = queryBuilder.obj();
            auto_ptr<DBClientCursor> cursor = db.query( ns.c_str() , query, num , skip );
            uassert( 13085 , "query failed for dbwebserver" , cursor.get() );

            if ( one ) {
                if ( cursor->more() ) {
                    BSONObj obj = cursor->next();
                    out << obj.jsonString(Strict,html?1:0) << '\n';
                }
                else {
                    responseCode = 404;
                }
                return html != 0;
            }

            if( html )  {
                string title = string("query ") + ns;
                out << start(title)
                    << p(title)
                    << "<pre>";
            }
            else {
                out << "{\n";
                out << "  \"offset\" : " << skip << ",\n";
                out << "  \"rows\": [\n";
            }

            int howMany = 0;
            while ( cursor->more() ) {
                if ( howMany++ && html == 0 )
                    out << " ,\n";
                BSONObj obj = cursor->next();
                if( html ) {
                    if( out.tellp() > 4 * 1024 * 1024 ) {
                        out << "Stopping output: more than 4MB returned and in html mode\n";
                        break;
                    }
                    out << obj.jsonString(Strict, html?1:0) << "\n\n";
                }
                else {
                    if( out.tellp() > 50 * 1024 * 1024 ) // 50MB limit - we are using ram
                        break;
                    out << "    " << obj.jsonString();
                }
            }

            if( html ) {
                out << "</pre>\n";
                if( howMany == 0 ) out << p("Collection is empty");
                out << _end();
            }
            else {
                out << "\n  ],\n\n";
                out << "  \"total_rows\" : " << howMany << " ,\n";
                out << "  \"query\" : " << query.jsonString() << " ,\n";
                out << "  \"millis\" : " << t.millis() << '\n';
                out << "}\n";
            }

            return html != 0;
        }

        // TODO Generate id and revision per couch POST spec
        void handlePost( string ns, const char *body, BSONObj& params, int & responseCode, stringstream & out ) {
            try {
                BSONObj obj = fromjson( body );
                db.insert( ns.c_str(), obj );
            }
            catch ( ... ) {
                responseCode = 400; // Bad Request.  Seems reasonable for now.
                out << "{ \"ok\" : false }";
                return;
            }

            responseCode = 201;
            out << "{ \"ok\" : true }";
        }

        int _getOption( BSONElement e , int def ) {
            if ( e.isNumber() )
                return e.numberInt();
            if ( e.type() == String )
                return atoi( e.valuestr() );
            return def;
        }

        DBDirectClient db;

    } restHandler;

    void openAdminDb() { 
        {
            readlocktry rl(/*"admin.system.users", */10000);
            uassert( 16172 , "couldn't get readlock to open admin db" , rl.got() );
            if( dbHolder().get("admin.system.users",dbpath) )
                return;
        }

        writelocktry wl(10000);
        verify( wl.got() );
        Client::Context cx( "admin.system.users", dbpath, false );
    }

    bool RestAdminAccess::haveAdminUsers() const {
        openAdminDb();
        readlocktry rl(/*"admin.system.users", */10000);
        uassert( 16173 , "couldn't get read lock to get admin auth credentials" , rl.got() );
        Client::Context cx( "admin.system.users", dbpath, false );
        return ! Helpers::isEmpty("admin.system.users", false);
    }

    BSONObj RestAdminAccess::getAdminUser( const string& username ) const {
        openAdminDb();
        Client::GodScope gs;
        readlocktry rl(/*"admin.system.users", */10000);
        uassert( 16174 , "couldn't get read lock to check admin user" , rl.got() );
        Client::Context cx( "admin.system.users" );
        BSONObj user;
        if ( Helpers::findOne( "admin.system.users" , BSON( "user" << username ) , user ) )
            return user.copy();
        return BSONObj();
    }

    class LowLevelMongodStatus : public WebStatusPlugin {
    public:
        LowLevelMongodStatus() : WebStatusPlugin( "overview" , 5 , "(only reported if can acquire read lock quickly)" ) {}

        virtual void init() {}

        void _gotLock( int millis , stringstream& ss ) {
            ss << "<pre>\n";
            ss << "time to get readlock: " << millis << "ms\n";
            ss << "# databases: " << dbHolder().sizeInfo() << '\n';
            ss << "# Cursors: " << ClientCursor::numCursors() << '\n';
            ss << "replication: ";
            if( *replInfo )
                ss << "\nreplInfo:  " << replInfo << "\n\n";
            if( replSet ) {
                ss << a("", "see replSetGetStatus link top of page") << "--replSet </a>" << cmdLine._replSet;
            }
            if ( replAllDead )
                ss << "\n<b>replication replAllDead=" << replAllDead << "</b>\n";
            else {
                ss << "\nmaster: " << replSettings.master << '\n';
                ss << "slave:  " << replSettings.slave << '\n';
                ss << '\n';
            }

            BackgroundOperation::dump(ss);
            ss << "</pre>\n";
        }

        virtual void run( stringstream& ss ) {
            Timer t;
            readlocktry lk( 300 );
            if ( lk.got() ) {
                _gotLock( t.millis() , ss );
            }
            else {
                ss << "\n<b>timed out getting lock</b>\n";
            }
        }
    } lowLevelMongodStatus;
}
