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

#include "storage.h"
#include "jsobj.h"
#include "namespace.h"

class DbMessage {
public:
	DbMessage(Message& _m) : m(_m) {
		theEnd = _m.data->_data + _m.data->dataLen();
		int *r = (int *) _m.data->_data;
		reserved = *r;
		r++;
		data = (const char *) r;
		nextjsobj = data;
	}

	const char * getns() { return data; }
	void getns(Namespace& ns) {
		ns = data;
	}

	int pullInt() {
		if( nextjsobj == data )
			nextjsobj += strlen(data) + 1; // skip namespace
		int i = *((int *)nextjsobj);
		nextjsobj += 4;
		return i;
	}
	long long pullInt64() {
		if( nextjsobj == data )
			nextjsobj += strlen(data) + 1; // skip namespace
		long long i = *((long long *)nextjsobj);
		nextjsobj += 8;
		return i;
	}

	OID* getOID() {
		return (OID *) (data + strlen(data) + 1); // skip namespace
	}

	void getQueryStuff(const char *&query, int& ntoreturn) {
		int *i = (int *) (data + strlen(data) + 1);
		ntoreturn = *i;
		i++;
		query = (const char *) i;
	}

	/* for insert and update msgs */
	bool moreJSObjs() { return nextjsobj != 0; }
	JSObj nextJsObj() {
		if( nextjsobj == data )
			nextjsobj += strlen(data) + 1; // skip namespace
		JSObj js(nextjsobj);
                assert( js.objsize() < ( theEnd - data ) );
		if( js.objsize() <= 0 )
			nextjsobj = null;
		else {
			nextjsobj += js.objsize();
			if( nextjsobj >= theEnd )
				nextjsobj = 0;
		}
		return js;
	}

private:
	Message& m;
	int reserved;
	const char *data;
	const char *nextjsobj;
	const char *theEnd;
};
