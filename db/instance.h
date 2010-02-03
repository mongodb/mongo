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
#include "curop.h"
#include "security.h"
#include "cmdline.h"
#include "client.h"

namespace mongo {

    extern string dbExecCommand;

#define OPWRITE if( _diaglog.level & 1 ) _diaglog.write((char *) m.data, m.data->len);
#define OPREAD if( _diaglog.level & 2 ) _diaglog.readop((char *) m.data, m.data->len);

    struct DiagLog {
        ofstream *f;
        /* 0 = off; 1 = writes, 2 = reads, 3 = both
           7 = log a few reads, and all writes.
        */
        int level;
        DiagLog() : f(0) , level(0) { }
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
            if ( level ) f->flush();
        }
        void write(char *data,int len) {
            if ( level & 1 ) f->write(data,len);
        }
        void readop(char *data, int len) {
            if ( level & 2 ) {
                bool log = (level & 4) == 0;
                OCCASIONALLY log = true;
                if ( log )
                    f->write(data,len);
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
        DbResponse(Message *r, MSGID rt) : response(r), responseTo(rt) {
        }
        DbResponse() {
            response = 0;
        }
        ~DbResponse() {
            delete response;
        }
    };

    static SockAddr unknownAddress( "0.0.0.0", 0 );
    
    bool assembleResponse( Message &m, DbResponse &dbresponse, const sockaddr_in &client = unknownAddress.sa );

    void receivedKillCursors(Message& m);
    void receivedUpdate(Message& m, stringstream& ss);
    void receivedDelete(Message& m, stringstream& ss);
    void receivedInsert(Message& m, stringstream& ss);
    bool receivedGetMore(DbResponse& dbresponse, Message& m, stringstream& ss);
    bool receivedQuery(DbResponse& dbresponse, Message& m, stringstream& ss, bool logit);
    void getDatabaseNames( vector< string > &names );

    // must call with db lock
    void registerListenerSocket( int socket );
    
// --- local client ---
    
    class DBDirectClient : public DBClientBase {
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
        class AlwaysAuthorized : public AuthenticationInfo {
            virtual bool isAuthorized( const char *dbname ) {
                return true;   
            }
        };
        /* TODO: this looks bad that auth is set to always.  is that really always safe? */
        class SavedContext {
        public:
            SavedContext() {
                dblock lk;
                Client *c = currentClient.get();
                if ( c->database() )
                    oldName = c->database()->name;
                oldAuth = c->ai;
                // careful, don't want to free this:
                c->ai = &always;
            }
            ~SavedContext() {
                Client *c = currentClient.get();
                c->ai = oldAuth;
                if ( !oldName.empty() ) {
                    dblock lk;
                    setClient( oldName.c_str() );
                }
            }
        private:
            static AlwaysAuthorized always;
            AuthenticationInfo *oldAuth;
            string oldName;
        };
    };

    extern int lockFile;
    void acquirePathLock();
    
} // namespace mongo
