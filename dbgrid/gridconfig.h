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

    class Grid;

    /**
       top level grid configuration for an entire database
    */
    class DBConfig : public Model {
    public:
        DBConfig( string name = "" ) : _name( name ) , _primary("") , _partitioned(false){ }

        /**
         * @return whether or not this partition is partitioned
         */
        bool partitioned( const NamespaceString& ns );
        
        /**
         * returns the correct for machine for the ns
         * if this namespace is partitioned, will return NULL
         */
        string getServer( const NamespaceString& ns );
        
        string getPrimary(){
            if ( _primary.size() == 0 )
                throw UserException( (string)"no primary server configured for db: " + _name );
            return _primary;
        }

        virtual string modelServer();

        // model stuff

        virtual const char * getNS(){ return "config.databases"; }
        virtual void serialize(BSONObjBuilder& to);
        virtual void unserialize(BSONObj& from);
        bool loadByName(const char *nm);

    protected:
        string _name; // e.g. "alleyinsider"
        string _primary; // e.g. localhost , mongo.foo.com:9999
        bool _partitioned;

        friend class Grid;
    };

    class Grid {
    public:
        /**
           gets the config the db.
           will return an empty DBConfig if not in db already
         */
        DBConfig * getDBConfig( string ns );
        
        string pickServerForNewDB();
        
    private:
        map<string,DBConfig*> _databases;
    };

    extern Grid grid;

} // namespace mongo
