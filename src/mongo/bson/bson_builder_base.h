/*    Copyright 2012 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
