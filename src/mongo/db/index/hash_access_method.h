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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
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

        /**
         * Hashing function used by both this class and the cursors we create.
         * Exposed for testing and so mongo/db/index_legacy.cpp can use it.
         */
        static long long int makeSingleKey(const BSONElement& e, HashSeed seed, int v);

        /**
         * Exposed externally for testing purposes.
         */
        static void getKeysImpl(const BSONObj& obj, const string& hashedField, HashSeed seed,
                                int hashVersion, bool isSparse, BSONObjSet* keys);

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
