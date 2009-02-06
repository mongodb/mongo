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

#include "../db/namespace.h"
#include "../client/dbclient.h"
#include "../client/model.h"

namespace mongo {

    /**
       abstract class for Models that need to talk to the config db
     */
    class GridConfigModel : public Model {
    public:
        virtual DBClientWithCommands* conn();
    };

    /**
       Machine is the concept of a host that runs the db process.
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

    /**
       top level grid configuration for an entire database
    */
    class DBConfig : public GridConfigModel {
    public:
        DBConfig() : _primary(0), _partitioned(false){ }

        /**
         * @return whether or not this partition is partitioned
         */
        bool partitioned( const NamespaceString& ns );
        
        /**
         * returns the correct for machine for the ns
         * if this namespace is partitioned, will return NULL
         */
        Machine * getMachine( const NamespaceString& ns );
        
        Machine * getPrimary(){
            uassert( "no primary" , _primary );
            return _primary;
        }

        // model stuff

        virtual const char * getNS(){ return "grid.db.database"; }
        virtual void serialize(BSONObjBuilder& to);
        virtual void unserialize(BSONObj& from);
        bool loadByName(const char *nm);

    protected:
        string _name; // e.g. "alleyinsider"
        Machine * _primary;
        bool _partitioned;
    };

    class Grid {
    public:
        /**
           gets the config the db.
           will return an empty DBConfig if not in db already
         */
        DBConfig * getDBConfig( string ns );

    private:
        map<string,DBConfig*> _databases;
    };

    extern Grid grid;

} // namespace mongo
