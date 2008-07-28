// repl.h - replication

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

/* replication data overview

   at the slave: 
     local.sources { host: ..., source: ..., syncedTo: }

   at the master:
     local.oplog.$<source>
     local.oplog.$main is the default
*/

#pragma once

extern bool slave;
extern bool master;

bool cloneFrom(const char *masterHost, string& errmsg);

#pragma pack(push)
#pragma pack(4)
class OpTime { 
      unsigned i;
      unsigned secs;
public:
      OpTime(unsigned a, unsigned b) { secs = a; i = b; }
      OpTime() { secs = 0; i = 0; }
      static OpTime now();
	  unsigned long long& asDate() { return *((unsigned long long *) this); } 
	  bool isNull() { return secs == 0; }
};
#pragma pack(pop)

/* A Source is a source from which we can pull (replicate) data.
   stored in collection local.sources.

   Can be a group of things to replicate for several databases.
*/
class Source {
public:
	string hostName;    // ip addr or hostname
	string sourceName;  // a logical source name.

	/* the last time point we have already synced up to. */
	OpTime syncedTo;

	static void loadAll(vector<Source*>&);
	static void cleanup(vector<Source*>&);
	Source(JSObj);
	void pull();
	void updateOnDisk();
	JSObj jsobj(); // { host: ..., source: ..., syncedTo: }
};

/* Write operation to the log (local.oplog.$main)
*/
void _logOp(const char *opstr, const char *ns, JSObj& obj, JSObj *patt, bool *b);
inline void logOp(const char *opstr, const char *ns, JSObj& obj, JSObj *patt = 0, bool *b = 0) {
	if( master )
		_logOp(opstr, ns, obj, patt, b);
}
