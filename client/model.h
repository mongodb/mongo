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
        virtual void unserialize(const BSONObj& from) = 0;

        virtual string modelServer() = 0;
        
        /** Load a single object. 
            @return true if successful.
        */
        virtual bool load(BSONObj& query);
        virtual void save( bool check=false );
        
    protected:
        BSONObj _id;
    };

} // namespace mongo
