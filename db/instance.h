// instance.h : Global state functions.
//

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

#pragma once


#include "../client/dbclient.h"
#include "curop-inl.h"
#include "security.h"
#include "cmdline.h"
#include "client.h"

namespace mongo {

    extern string dbExecCommand;

    struct DiagLog {
        ofstream *f;
        /* 0 = off; 1 = writes, 2 = reads, 3 = both
           7 = log a few reads, and all writes.
        */
        int level;
        mongo::mutex mutex;

        DiagLog() : f(0) , level(0), mutex("DiagLog") { }
        void init() {
            if ( ! f && level ){
                log() << "diagLogging = " << level << endl;
                stringstream ss;
                ss << dbpath << "/diaglog." << hex << time(0);
                string name = ss.str();
                f = new ofstream(name.c_str(), ios::out | ios::binary);
                if ( ! f->good() ) {
                    problem() << "couldn't open log stream" << endl;
                    throw 1717;
                }
            }
        }
        /**
         * @return old
         */
        int setLevel( int newLevel ){
            int old = level;
            level = newLevel;
            init();
            return old;
        }
        void flush() {
            if ( level ){
                scoped_lock lk(mutex);
                f->flush();
            }
        }
        void write(char *data,int len) {
            if ( level & 1 ){
                scoped_lock lk(mutex);
                f->write(data,len);
            }
        }
        void readop(char *data, int len) {
            if ( level & 2 ) {
                bool log = (level & 4) == 0;
                OCCASIONALLY log = true;
                if ( log ){
                    scoped_lock lk(mutex);
                    assert( f );
                    f->write(data,len);
                }
            }
        }
    };

    extern DiagLog _diaglog;

    /* we defer response until we unlock.  don't want a blocked socket to
       keep things locked.
    */
    struct DbResponse {
        Message *response;
        MSGID responseTo;
        const char *exhaust; /* points to ns if exhaust mode. 0=normal mode*/
        DbResponse(Message *r, MSGID rt) : response(r), responseTo(rt), exhaust(0) { }
        DbResponse() {
            response = 0;
            exhaust = 0;
        }
        ~DbResponse() { delete response; }
    };
    
    bool assembleResponse( Message &m, DbResponse &dbresponse, const SockAddr &client = unknownAddress );

    void getDatabaseNames( vector< string > &names , const string& usePath = dbpath );

    /* returns true if there is no data on this server.  useful when starting replication. 
       local database does NOT count. 
    */
    bool replHasDatabases();

    /** "embedded" calls to the local server directly. */
    class DBDirectClient : public DBClientBase {        
    public:
        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0);
        
        virtual bool isFailed() const {
            return false;
        }
        virtual string toString() {
            return "DBDirectClient";
        }
        virtual string getServerAddress() const{
            return "localhost"; // TODO: should this have the port?
        }
        virtual bool call( Message &toSend, Message &response, bool assertOk=true );
        virtual void say( Message &toSend );
        virtual void sayPiggyBack( Message &toSend ) {
            // don't need to piggy back when connected locally
            return say( toSend );
        }
        
        virtual void killCursor( long long cursorID );
        
        virtual bool callRead( Message& toSend , Message& response ){
            return call( toSend , response );
        }

        virtual ConnectionString::ConnectionType type() const { return ConnectionString::MASTER; }  
    };

    extern int lockFile;
    void acquirePathLock();
    void maybeCreatePidFile();
    
} // namespace mongo
