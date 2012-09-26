/*    Copyright 2012 10gen Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"

namespace mongo {

    /*
     * BSONBuilderBase contains the common interface between different types of BSON builders.
     *
     * Currently, this interface is not comprehensive. But we should work toward that goal. If
     * you are maintaining the class, please make sure that all Builders behave coherently.
     */
    class BSONBuilderBase {
    public:
        virtual ~BSONBuilderBase() {}

        virtual BSONObj obj() = 0;

        virtual BufBuilder& subobjStart( const StringData& fieldName ) = 0;

        virtual BufBuilder& subarrayStart( const StringData& fieldName ) = 0;

        virtual BSONBuilderBase& append( const BSONElement& e) = 0;

        virtual BSONBuilderBase& append( const StringData& fieldName , int n ) = 0;

        virtual BSONBuilderBase& append( const StringData& fieldName , long long n ) = 0;

        virtual BSONBuilderBase& append( const StringData& fieldName , double n ) = 0;

        virtual BSONBuilderBase& appendArray( const StringData& fieldName , const BSONObj& subObj ) = 0;

        virtual BSONBuilderBase& appendAs( const BSONElement& e , const StringData& filedName ) = 0;

        virtual void appendNull( ) = 0;

        virtual BSONBuilderBase& operator<<( const BSONElement& e ) = 0;

        virtual bool isArray() const = 0;
    };

} // namespace mongo
