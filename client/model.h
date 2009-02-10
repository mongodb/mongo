/** @file model.h */

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

#include "dbclient.h"

namespace mongo {

    /** Model is a base class for defining objects which are serializable to the Mongo
       database via the database driver.

       Definition
       Your serializable class should inherit from Model and implement the abstract methods
       below.

       Loading
       To load, first construct an (empty) object.  Then call load().  Do not load an object
       more than once.
    */
    class Model {
    public:
        Model() { }
        virtual ~Model() { }

        virtual const char * getNS() = 0;
        virtual void serialize(BSONObjBuilder& to) = 0;
        virtual void unserialize(BSONObj& from) = 0;

        /** Define this as you see fit if you are using the default conn() implementation. */
        static string defaultServer;

        /** Override this if you need to do fancier connection management than simply using globalConn. */
        virtual string modelServer(){
            uassert( "defaultServer not set" , defaultServer.size() );
            return defaultServer;
        }

        /** Load a single object. 
            @return true if successful.
        */
        bool load(BSONObj& query);
        void save();
        
    private:
        BSONElement _id;
    };

} // namespace mongo
