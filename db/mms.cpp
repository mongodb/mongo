// mms.cpp

#include "stdafx.h"
#include "mms.h"
#include "db.h"
#include "instance.h"
#include "../util/httpclient.h"

namespace mongo {
    
    MMS::MMS() : 
        baseurl( "http://mms.10gen.com/ping/" ) , secsToSleep(1) , token( "" ) , name( "" ) {
    }
    
    MMS::~MMS(){
        
    }
    
    void MMS::run(){
        
        if ( token.size() == 0  && name.size() == 0 ){
            log(1) << "mms not configured" << endl;
            return;
        }

        if ( token.size() == 0 ){
            log() << "no token for mms - not running" << endl;
            return;
        }
        
        if ( name.size() == 0 ){
            log() << "no name for mms - not running" << endl;
            return;
        }

        log() << "mms monitor staring...  token:" << token << " name:" << name << " interval: " << secsToSleep << endl;

        unsigned long long lastTime = 0;
        unsigned long long lastLockTime = 0;
        
        while ( ! inShutdown() ){
            sleepsecs( secsToSleep );
            
            stringstream url;
            url << baseurl << token << "?";
            url << "monitor_name=" << name << "&";
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


    MMS mms;    
}
