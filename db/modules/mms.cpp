// mms.cpp

#include "stdafx.h"
#include "../db.h"
#include "../instance.h"
#include "../module.h"
#include "../../util/httpclient.h"
#include "../../util/background.h"

namespace po = boost::program_options;

namespace mongo {

    /** Mongo Monitoring Service
        if enabled, this runs in the background ands pings mss
     */
    class MMS : public BackgroundJob , Module {
    public:

        MMS()
            : Module( "mms" ) , _baseurl( "http://mms.10gen.com/ping/" ) , 
              _secsToSleep(1) , _token( "" ) , _name( "" ) {
            
            add_options()
                ( "mms-token" , po::value<string>() , "account token for mongo monitoring server" )
                ( "mms-name" , po::value<string>() , "server name mongo monitoring server" )
                ( "mms-interval" , po::value<int>()->default_value(30) , "ping interval for mongo monitoring server" )
                ;
        }    
        
        ~MMS(){}

        void config( program_options::variables_map& params ){
            if ( params.count( "mms-token" ) ){
                _token = params["mms-token"].as<string>();
            }
            if ( params.count( "mms-name" ) ){
                _name = params["mms-name"].as<string>();
            }
            _secsToSleep = params["mms-interval"].as<int>();
        }
        
        void run(){
        if ( _token.size() == 0  && _name.size() == 0 ){
            log(1) << "mms not configured" << endl;
            return;
        }

        if ( _token.size() == 0 ){
            log() << "no token for mms - not running" << endl;
            return;
        }
        
        if ( _name.size() == 0 ){
            log() << "no name for mms - not running" << endl;
            return;
        }

        log() << "mms monitor staring...  token:" << _token << " name:" << _name << " interval: " << _secsToSleep << endl;

        unsigned long long lastTime = 0;
        unsigned long long lastLockTime = 0;
        
        while ( ! inShutdown() ){
            sleepsecs( _secsToSleep );
            
            stringstream url;
            url << _baseurl << _token << "?";
            url << "monitor_name=" << _name << "&";
            url << "version=" << versionString << "&";
            url << "git_hash=" << gitVersion() << "&";

            { //percent_locked
                unsigned long long time = curTimeMicros64();
                unsigned long long start , lock;
                dbMutexInfo.timingInfo( start , lock );
                if ( lastTime ){
                    double timeDiff = (double) (time - lastTime);
                    double lockDiff = (double) (lock - lastLockTime);
                    url << "percent_locked=" << (int)ceil( 100 * ( lockDiff / timeDiff ) ) << "&";
                }
                lastTime = time;
                lastLockTime = lock;
            }
            
            vector< string > dbNames;
            getDatabaseNames( dbNames );
            boost::intmax_t totalSize = 0;
            for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
                boost::intmax_t size = dbSize( i->c_str() );
                totalSize += size;
            }
            url << "data_size=" << totalSize / ( 1024 * 1024 ) << "&";

            
            
            /* TODO: 
              message_operations
              update_operations
              insert_operations
              get_more_operations
              delete_operations
              kill_cursors_operations 
            */
            

            log(1) << "mms url: " << url.str() << endl;
            
            try {
                HttpClient c;
                map<string,string> headers;
                stringstream ss;
                int rc = c.get( url.str() , headers , ss );
                log(1) << "\t response code: " << rc << endl;
                if ( rc != 200 ){
                    log() << "mms error response code:" << rc << endl;
                    log(1) << "mms error body:" << ss.str() << endl;
                }
            }
            catch ( std::exception& e ){
                log() << "mms get exception: " << e.what() << endl;
            }
        }
        }

        void init(){ go(); }

        void shutdown(){
            // TODO
        }

    private:
        string _baseurl;
        int _secsToSleep;
        
        string _token;
        string _name;

    } /* mms */;

}

        

