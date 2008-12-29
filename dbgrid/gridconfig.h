// gridconfig.h

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

/* This file is things related to the "grid configuration":
   - what machines make up the db component of our cloud
   - where various ranges of things live
*/

#pragma once

#include "../client/dbclient.h"
#include "../client/model.h"
#include "griddatabase.h"

/* Machine is the concept of a host that runs the db process.
*/
class Machine {
    static map<string, Machine*> machines;
    string name;
public:
    string getName() const {
        return name;
    }

    Machine(string _name) : name(_name) { }

    enum {
        Port = 27018 /* default port # for dbs that are downstream of a dbgrid */
    };

    static Machine* get(string name) {
        map<string,Machine*>::iterator i = machines.find(name);
        if ( i != machines.end() )
            return i->second;
        return machines[name] = new Machine(name);
    }
};

//typedef map<string,Machine*> ObjLocs;

/* top level grid configuration for an entire database */
class ClientConfig : public Model {
public:
    string name; // e.g. "alleyinsider"
    Machine *primary;
    bool partitioned;

    ClientConfig() : primary(0), partitioned(false) { }

    virtual const char * getNS() {
        return "grid.db.database";
    }
    virtual void serialize(BSONObjBuilder& to) {
        to.append("name", name);
        to.appendBool("partitioned", partitioned);
        if ( primary )
            to.append("primary", primary->getName());
    }
    virtual void unserialize(BSONObj& from) {
        name = from.getStringField("name");
        partitioned = from.getBoolField("partitioned");
        string p = from.getStringField("primary");
        if ( !p.empty() )
            primary = Machine::get(p);
    }

    bool loadByName(const char *nm) {
        BSONObjBuilder b;
        b.append("name", nm);
        BSONObj q = b.done();
        return load(q);
    }
};

class GridConfig {
    map<string,ClientConfig*> databases;
public:
    ClientConfig* getClientConfig(string database);
};

class Grid {
    GridConfig gc;
public:
    /* return which machine "owns" the object in question -- ie which partition
       we should go to.
           */
    Machine* owner(const char *ns, BSONObj& objOrKey);
};

extern Grid grid;
