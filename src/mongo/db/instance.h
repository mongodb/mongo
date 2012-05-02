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


#include "curop-inl.h"
#include "security.h"
#include "cmdline.h"
#include "client.h"
#include "mongo/client/dbclientinterface.h"

namespace mongo {

    extern string dbExecCommand;

    /** a high level recording of operations to the database - sometimes used for diagnostics 
        and debugging.
        */
    class DiagLog {
        ofstream *f; // note this is never freed
        /* 0 = off; 1 = writes, 2 = reads, 3 = both
           7 = log a few reads, and all writes.
        */
        int level;
        mongo::mutex mutex;
        void openFile();

    public:
        DiagLog();
        int getLevel() const { return level; }
        /**
         * @return old
         */
        int setLevel( int newLevel );
        void flush();
        void writeop(char *data,int len);
        void readop(char *data, int len);
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

    void assembleResponse( Message &m, DbResponse &dbresponse, const HostAndPort &client );

    void getDatabaseNames( vector< string > &names , const string& usePath = dbpath );

    /* returns true if there is no data on this server.  useful when starting replication.
       local database does NOT count.
    */
    bool replHasDatabases();

    /** "embedded" calls to the local server directly. 
        Caller does not need to lock, that is handled within.
     */
    class DBDirectClient : public DBClientBase {
    public:
        using DBClientBase::query;

        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0, int batchSize = 0);

        virtual bool isFailed() const {
            return false;
        }
        virtual string toString() {
            return "DBDirectClient";
        }
        virtual string getServerAddress() const {
            return "localhost"; // TODO: should this have the port?
        }
        virtual bool call( Message &toSend, Message &response, bool assertOk=true , string * actualServer = 0 );
        virtual void say( Message &toSend, bool isRetry = false , string * actualServer = 0 );
        virtual void sayPiggyBack( Message &toSend ) {
            // don't need to piggy back when connected locally
            return say( toSend );
        }

        virtual void killCursor( long long cursorID );

        virtual bool callRead( Message& toSend , Message& response ) {
            return call( toSend , response );
        }
        
        virtual unsigned long long count(const string &ns, const BSONObj& query = BSONObj(), int options=0, int limit=0, int skip=0 );
        
        virtual ConnectionString::ConnectionType type() const { return ConnectionString::MASTER; }

        double getSoTimeout() const { return 0; }

        virtual bool lazySupported() const { return true; }

        virtual QueryOptions _lookupAvailableOptions();

    private:
        static HostAndPort _clientHost;
    };

    extern int lockFile;
#ifdef _WIN32
    extern HANDLE lockFileHandle;
#endif
    void acquirePathLock(bool doingRepair=false); // if doingRepair=true don't consider unclean shutdown an error
    void maybeCreatePidFile();

    void exitCleanly( ExitCode code );

} // namespace mongo
