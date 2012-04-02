/** @file model.h */

/*    Copyright 2009 10gen
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"

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
        virtual BSONObj toObject();
        virtual void append( const char * name , BSONObjBuilder& b );

        virtual string modelServer() = 0;

        /** Load a single object.
            @return true if successful.
        */
        virtual bool load(BSONObj& query);
        virtual void save( bool safe=false );
        virtual void remove( bool safe=false );

    protected:
        BSONObj _id;
    };

} // namespace mongo

