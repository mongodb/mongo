// dbwebserver.cpp

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
#include "../util/miniwebserver.h"
#include "db.h"
#include "repl.h"
#include "replset.h"

extern int port;
extern const char *replInfo;

bool getInitialSyncCompleted();

time_t started = time(0);

/*
    string toString() {
        stringstream ss;
        unsigned long long dt = last - start;
        ss << dt/1000;
        ss << '\t';
        ss << timeLocked/1000 << '\t';
        if( dt )
            ss << (timeLocked*100)/dt << '%';
        return ss.str();
    }
*/

struct Timing {
    Timing() {
        start = timeLocked = 0;
    }
    unsigned long long start, timeLocked;
};
Timing tlast;
const int NStats = 32;
string lockStats[NStats];
unsigned q = 0;
extern bool quiet;

void statsThread() {
    unsigned long long timeLastPass = 0;
    while ( 1 ) {
        {
            Timer lktm;
            dblock lk;
            q = (q+1)%NStats;
            Timing timing;
            dbMutexInfo.timingInfo(timing.start, timing.timeLocked);
            unsigned long long now = curTimeMicros64();
            if ( timeLastPass ) {
                unsigned long long dt = now - timeLastPass;
                unsigned long long dlocked = timing.timeLocked - tlast.timeLocked;
                {
                    stringstream ss;
                    ss << dt / 1000 << '\t';
                    ss << dlocked / 1000 << '\t';
                    if ( dt )
                        ss << (dlocked*100)/dt << '%';
                    string s = ss.str();
                    if ( !quiet )
                        log() << "cpu: " << s << endl;
                    lockStats[q] = s;
                }
            }
            timeLastPass = now;
            tlast = timing;
        }
        sleepsecs(4);
    }
}

unsigned byLocSize();

bool _bold;
string bold(bool x) {
    _bold = x;
    return x ? "<b>" : "";
}
string bold() {
    return _bold ? "</b>" : "";
}

class DbWebServer : public MiniWebServer {
public:
    // caller locks
    void doLockedStuff(stringstream& ss) {
        ss << "# databases: " << databases.size() << '\n';
        if ( database ) {
            ss << "curclient: " << database->name;
            ss << '\n';
        }
        ss << bold(byLocSize()>10000) << "Cursors byLoc.size(): " << byLocSize() << bold() << '\n';
        ss << "\n<b>replication</b>\n";
        ss << "master: " << master << '\n';
        ss << "slave:  " << slave << '\n';
        if ( replPair ) {
            ss << "replpair:\n";
            ss << replPair->getInfo();
        }
        bool seemCaughtUp = getInitialSyncCompleted();
        if ( !seemCaughtUp ) ss << "<b>";
        ss <<   "initialSyncCompleted: " << seemCaughtUp;
        if ( !seemCaughtUp ) ss << "</b>";
        ss << '\n';

        ss << "\n<b>dt\ttlocked</b>\n";
        unsigned i = q;
        while ( 1 ) {
            ss << lockStats[i] << '\n';
            i = (i-1)%NStats;
            if ( i == q )
                break;
        }
    }

    void doUnlockedStuff(stringstream& ss) {
        ss << "port:      " << port << '\n';
        ss << "dblocked:  " << dbMutexInfo.isLocked() << " (initial)\n";
        ss << "uptime:    " << time(0)-started << " seconds\n";
        if ( allDead )
            ss << "<b>replication allDead=" << allDead << "</b>\n";
        ss << "\nassertions:\n";
        for ( int i = 0; i < 4; i++ ) {
            if ( lastAssert[i].isSet() ) {
                ss << "<b>";
                if ( i == 3 ) ss << "usererr";
                else ss << i;
                ss << "</b>" << ' ' << lastAssert[i].toString();
            }
        }

        ss << "\nreplInfo:  " << replInfo << '\n';
    }

    virtual void doRequest(
        const char *rq, // the full request
        string url,
        // set these and return them:
        string& responseMsg,
        int& responseCode,
        vector<string>& headers // if completely empty, content-type: text/html will be added
    )
    {
        //cout << "url [" << url << "]" << endl;
        
        if ( url.size() > 1 ){
            handleRESTRequest( rq , url , responseMsg , responseCode , headers );
            return;
        }
            
        
        responseCode = 200;
        stringstream ss;
        ss << "<html><head><title>";

        string dbname;
        {
            stringstream z;
            z << "db " << getHostName() << ':' << port << ' ';
            dbname = z.str();
        }
        ss << dbname << "</title></head><body><h2>" << dbname << "</h2><p>\n<pre>";

        doUnlockedStuff(ss);

        int n = 2000;
        Timer t;
        while ( 1 ) {
            if ( !dbMutexInfo.isLocked() ) {
                {
                    dblock lk;
                    ss << "time to get dblock: " << t.millis() << "ms\n";
                    doLockedStuff(ss);
                }
                break;
            }
            sleepmillis(1);
            if ( --n < 0 ) {
                ss << "\n<b>timed out getting dblock</b>\n";
                break;
            }
        }

        ss << "</pre></body></html>";
        responseMsg = ss.str();
    }
    
    void handleRESTRequest( const char *rq, // the full request
                            string url,
                            string& responseMsg,
                            int& responseCode,
                            vector<string>& headers // if completely empty, content-type: text/html will be added
                            ){
        
        string::size_type first = url.find( "/" , 1 );
        if ( first == string::npos ){
            responseCode = 400;
            return;
        }

        string dbname = url.substr( 1 , first - 1 );
        string coll = url.substr( first + 1 );
        string action = "";
        
        string::size_type last = coll.find_last_of( "/" );
        if ( last == string::npos ){
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

        string fullns = dbname + "." + coll;
        

        headers.push_back( (string)"x-dbname: " + dbname );
        headers.push_back( (string)"x-coll: " + coll );
        headers.push_back( (string)"x-action: " + action );
        headers.push_back( (string)"x-fullns: " + fullns );

        responseCode = 405;
        
    }
    
        
};

void webServerThread() {
    boost::thread thr(statsThread);
    DbWebServer mini;
    if ( mini.init(port+1000) )
        mini.run();
}
