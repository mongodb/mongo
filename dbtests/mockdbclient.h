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

class MockDBClientConnection : public DBClientConnection {
public:
	MockDBClientConnection() : connect_() {}
	virtual
    BSONObj findOne(const char *ns, BSONObj query, BSONObj *fieldsToReturn = 0, int queryOptions = 0) {
		return one_;
	}
	virtual
    bool connect(const char *serverHostname, string& errmsg) {
		return connect_;
	}
	virtual
    BSONObj cmdIsMaster(bool& isMaster) {
		return res_;
	}
	void one( const BSONObj &one ) { one_ = one; }
	void connect( bool val ) { connect_ = val; }
	void res( const BSONObj &val ) { res_ = val; }
private:
	BSONObj one_;
	bool connect_;
	BSONObj res_;
};

class DirectDBClientConnection : public DBClientConnection {
public:
	DirectDBClientConnection( ReplPair *rp ) : rp_( rp ) {
	}
	virtual BSONObj findOne(const char *ns, BSONObj query, BSONObj *fieldsToReturn = 0, int queryOptions = 0) {
		SetGlobalReplPair s( rp_ );
		BSONObjBuilder result;
		result.append( "ok", runCommandAgainstRegistered( "admin.$cmd", query, result ) ? 1.0 : 0.0 );
		return result.doneAndDecouple();
	}
	virtual bool connect( const char *serverHostname, string& errmsg ) {
		return true;
	}
private:
	ReplPair *rp_;
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
