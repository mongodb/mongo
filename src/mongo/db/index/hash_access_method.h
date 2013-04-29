/**
*    Copyright (C) 2013 10gen Inc.
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

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/hasher.h"  // For HashSeed.
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/btree_access_method_internal.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * This is the access method for "hashed" indices.
     */
    class HashAccessMethod : public BtreeBasedAccessMethod {
    public:
        using BtreeBasedAccessMethod::_descriptor;

        HashAccessMethod(IndexDescriptor* descriptor);
        virtual ~HashAccessMethod() { }

        virtual Status newCursor(IndexCursor** out);

        // This is a NO-OP.
        virtual Status setOptions(const CursorOptions& options) {
            return Status::OK();
        }

        // Our missing field is different than the default missing field, this needs to be
        // exposed in s/d_split.cpp.  That's the only thing that calls this.
        static BSONObj getMissingField(const IndexDetails& details);

        /**
         * Hashing function used by both this class and the cursors we create.
         */
        static long long int makeSingleKey(const BSONElement& e, HashSeed seed, int v);

    private:
        virtual void getKeys(const BSONObj& obj, BSONObjSet* keys);

        // Only one of our fields is hashed.  This is the field name for it.
        string _hashedField;

        // _seed defaults to zero.
        HashSeed _seed;

        // _hashVersion defaults to zero.
        int _hashVersion;

        BSONObj _missingKey;
    };

}  // namespace mongo
