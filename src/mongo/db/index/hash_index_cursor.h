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

#include <boost/scoped_ptr.hpp>
#include <vector>

#include "mongo/db/btreecursor.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    class HashIndexCursor : public IndexCursor {
    public:
        HashIndexCursor(const string& hashedField, HashSeed seed, int hashVersion,
                        IndexDescriptor* descriptor);

        virtual ~HashIndexCursor() { }

        virtual Status setOptions(const CursorOptions& options);

        virtual Status seek(const BSONObj &position);

        bool isEOF() const;
        BSONObj getKey() const;
        DiskLoc getValue() const;
        void next();
        string toString();

        Status savePosition();
        Status restorePosition();

        void aboutToDeleteBucket(const DiskLoc& bucket);

    private:
        string _hashedField;
        scoped_ptr<BtreeCursor> _oldCursor;

        // Default of zero.
        HashSeed _seed;

        // Default of zero.
        int _hashVersion;
        IndexDescriptor *_descriptor;
    };

}  // namespace mongo
