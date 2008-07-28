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
*/

#pragma once

bool cloneFrom(const char *masterHost, string& errmsg);

#pragma pack(push)
#pragma pack(4)
class OpTime { 
      unsigned secs;
      unsigned i;
public:
      OpTime(unsigned a, unsigned b) { secs = a; i = b; }
      OpTime() { secs = 0; i = 0; }
      static OpTime now();
	  double& asDouble() { return *((double *) this); } 
	  bool isNull() { return secs == 0; }
};
#pragma pack(pop)

/* A Source is a source from which we can pull (replicate) data.
   stored in collection local.sources.

   Can be a group of things to replicate for several databases.
*/
class Source {
public:
	string hostName;
	string sourceName;
	OpTime syncedTo;
	static void loadAll(vector<Source*>&);
	static void cleanup(vector<Source*>&);
	Source(JSObj);
	void pull();
	void updateOnDisk();
	JSObj jsobj(); // { host: ..., source: ..., syncedTo: }
};
