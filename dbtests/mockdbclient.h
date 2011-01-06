// mockdbclient.h - mocked out client for testing.

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
#include "../db/commands.h"
#include "../db/replpair.h"

class MockDBClientConnection : public DBClientConnection {
public:
    MockDBClientConnection() : connect_() {}
    virtual
    BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0) {
        return one_;
    }
    virtual
    bool connect(const char * serverHostname, string& errmsg) {
        return connect_;
    }
    virtual
    bool connect(const HostAndPort& , string& errmsg) {
        return connect_;
    }
    virtual
    bool isMaster(bool& isMaster, BSONObj *info=0) {
        return isMaster_;
    }
    void one( const BSONObj &one ) {
        one_ = one;
    }
    void connect( bool val ) {
        connect_ = val;
    }
    void setIsMaster( bool val ) {
        isMaster_ = val;
    }
private:
    BSONObj one_;
    bool connect_;
    bool isMaster_;
};

class DirectDBClientConnection : public DBClientConnection {
public:
    struct ConnectionCallback {
        virtual ~ConnectionCallback() {}
        virtual void beforeCommand() {}
        virtual void afterCommand() {}
    };
    DirectDBClientConnection( ReplPair *rp, ConnectionCallback *cc = 0 ) :
        rp_( rp ),
        cc_( cc ) {
    }
    virtual BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0) {
        BSONObj c = query.obj.copy();
        if ( cc_ ) cc_->beforeCommand();
        SetGlobalReplPair s( rp_ );
        BSONObjBuilder result;
        result.append( "ok", Command::runAgainstRegistered( "admin.$cmd", c, result ) ? 1.0 : 0.0 );
        if ( cc_ ) cc_->afterCommand();
        return result.obj();
    }
    virtual bool connect( const string &serverHostname, string& errmsg ) {
        return true;
    }
private:
    ReplPair *rp_;
    ConnectionCallback *cc_;
    class SetGlobalReplPair {
    public:
        SetGlobalReplPair( ReplPair *rp ) {
            backup_ = replPair;
            replPair = rp;
        }
        ~SetGlobalReplPair() {
            replPair = backup_;
        }
    private:
        ReplPair *backup_;
    };
};
