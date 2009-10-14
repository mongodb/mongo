// mms.h

#pragma once

#include "../stdafx.h"
#include "../util/background.h"

namespace mongo {


    /** Mongo Monitoring Service
        if enabled, this runs in the background ands pings mss
     */
    class MMS : public BackgroundJob {
    public:

        MMS();
        ~MMS();

        /**
           e.g. http://mms.10gen.com/ping/
         */
        void setBaseUrl( const string& host );
        
        void setToken( const string& s ){ token = s; }
        void setName( const string& s ){ name = s; }

        void setPingInterval( int seconds ){ secsToSleep = seconds; }

        void run();

    private:
        string baseurl;
        int secsToSleep;
        
        string token;
        string name;

    };

    extern MMS mms;
}
