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

namespace mongo {

    extern CurOp currentOp;
    
// turn on or off the oplog.* files which the db can generate.
// these files are for diagnostic purposes and are unrelated to
// local.oplog.$main used by replication.
//
#define OPLOG if( 0 )

    int getOpLogging();

    extern string dbExecCommand;

#define OPWRITE if( getOpLogging() & 1 ) _oplog.write((char *) m.data, m.data->len);
#define OPREAD if( getOpLogging() & 2 ) _oplog.readop((char *) m.data, m.data->len);

    struct OpLog {
        ofstream *f;
        OpLog() : f(0) { }
        void init() {
            OPLOG {
                stringstream ss;
                ss << "oplog." << hex << time(0);
                string name = ss.str();
                f = new ofstream(name.c_str(), ios::out | ios::binary);
                if ( ! f->good() ) {
                    problem() << "couldn't open log stream" << endl;
                    throw 1717;
                }
            }
        }
        void flush() {
            OPLOG f->flush();
        }
        void write(char *data,int len) {
            OPLOG f->write(data,len);
        }
        void readop(char *data, int len) {
            OPLOG {
                bool log = (getOpLogging() & 4) == 0;
                OCCASIONALLY log = true;
                if ( log )
                    f->write(data,len);
            }
        }
    };

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

    bool assembleResponse( Message &m, DbResponse &dbresponse );

    void receivedKillCursors(Message& m);
    void receivedUpdate(Message& m, stringstream& ss);
    void receivedDelete(Message& m);
    void receivedInsert(Message& m, stringstream& ss);
    void receivedGetMore(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss);
    void receivedQuery(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss, bool logit);
    void getDatabaseNames( vector< string > &names );


// --- local client ---
    
    class DBDirectClient : public DBClientBase {
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
        class Authorizer {
        public:
            Authorizer() {
                backup_.reset( authInfo.release() );
                // careful, don't want to free this.
                authInfo.reset( &always );
            }
            ~Authorizer() {
                authInfo.release();
                authInfo.reset( backup_.release() );
            }
        private:
            static AlwaysAuthorized always;
            boost::thread_specific_ptr< AuthenticationInfo > backup_;
        };
    };
    
} // namespace mongo
