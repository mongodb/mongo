// @file mms.cpp
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


#include "pch.h"
#include "../db.h"
#include "../instance.h"
#include "../module.h"
#include "../../util/net/httpclient.h"
#include "../../util/background.h"
#include "../commands.h"

namespace po = boost::program_options;

namespace mongo {

    /** Mongo Monitoring Service
        if enabled, this runs in the background ands pings mss
    */
    class MMS : public BackgroundJob , Module {
    public:

        MMS()
            : Module( "mms" ) , _baseurl( "" ) ,
              _secsToSleep(1) , _token( "" ) , _name( "" ) {

            add_options()
            ( "mms-url" , po::value<string>()->default_value("http://mms.10gen.com/ping") , "url for mongo monitoring server" )
            ( "mms-token" , po::value<string>() , "account token for mongo monitoring server" )
            ( "mms-name" , po::value<string>() , "server name for mongo monitoring server" )
            ( "mms-interval" , po::value<int>()->default_value(30) , "ping interval (in seconds) for mongo monitoring server" )
            ;
        }

        ~MMS() {}

        void config( boost::program_options::variables_map& params ) {
            _baseurl = params["mms-url"].as<string>();
            if ( params.count( "mms-token" ) ) {
                _token = params["mms-token"].as<string>();
            }
            if ( params.count( "mms-name" ) ) {
                _name = params["mms-name"].as<string>();
            }
            _secsToSleep = params["mms-interval"].as<int>();
        }

        void run() {
            if ( _token.size() == 0  && _name.size() == 0 ) {
                log(1) << "mms not configured" << endl;
                return;
            }

            if ( _token.size() == 0 ) {
                log() << "no token for mms - not running" << endl;
                return;
            }

            if ( _name.size() == 0 ) {
                log() << "no name for mms - not running" << endl;
                return;
            }

            log() << "mms monitor staring...  token:" << _token << " name:" << _name << " interval: " << _secsToSleep << endl;
            Client::initThread( "mms" );
            Client& c = cc();


            // TODO: using direct client is bad, but easy for now

            while ( ! inShutdown() ) {
                sleepsecs( _secsToSleep );

                try {
                    stringstream url;
                    url << _baseurl << "?"
                        << "token=" << _token << "&"
                        << "name=" << _name << "&"
                        << "ts=" << time(0)
                        ;

                    BSONObjBuilder bb;
                    // duplicated so the post has everything
                    bb.append( "token" , _token );
                    bb.append( "name" , _name );
                    bb.appendDate( "ts" , jsTime()  );

                    // any commands
                    _add( bb , "buildinfo" );
                    _add( bb , "serverStatus" );

                    BSONObj postData = bb.obj();

                    log(1) << "mms url: " << url.str() << "\n\t post: " << postData << endl;;

                    HttpClient c;
                    HttpClient::Result r;
                    int rc = c.post( url.str() , postData.jsonString() , &r );
                    log(1) << "\t response code: " << rc << endl;
                    if ( rc != 200 ) {
                        log() << "mms error response code:" << rc << endl;
                        log(1) << "mms error body:" << r.getEntireResponse() << endl;
                    }
                }
                catch ( std::exception& e ) {
                    log() << "mms exception: " << e.what() << endl;
                }
            }

            c.shutdown();
        }

        void _add( BSONObjBuilder& postData , const char* cmd ) {
            Command * c = Command::findCommand( cmd );
            if ( ! c ) {
                log() << "MMS can't find command: " << cmd << endl;
                postData.append( cmd , "can't find command" );
                return;
            }

            if ( c->locktype() ) {
                log() << "MMS can only use noLocking commands not: " << cmd << endl;
                postData.append( cmd , "not noLocking" );
                return;
            }

            BSONObj co = BSON( cmd << 1 );

            string errmsg;
            BSONObjBuilder sub;
            if ( ! c->run( "admin.$cmd" , co , 0 , errmsg , sub , false ) )
                postData.append( cmd , errmsg );
            else
                postData.append( cmd , sub.obj() );
        }


        void init() { go(); }

        void shutdown() {
            // TODO
        }

    private:
        string _baseurl;
        int _secsToSleep;

        string _token;
        string _name;

    } /*mms*/ ;

}



