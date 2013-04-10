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

#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/index/btree_interface.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

    class BtreeIndexCursor : public IndexCursor {
    public:
        virtual ~BtreeIndexCursor() { }

        bool isEOF() const;

        // XXX SHORT TERM HACKS THAT MUST DIE: 2d index
        virtual DiskLoc getBucket() const;

        // XXX SHORT TERM HACKS THAT MUST DIE: 2d index
        virtual int getKeyOfs() const;

        // XXX SHORT TERM HACKS THAT MUST DIE: btree deletion
        virtual void aboutToDeleteBucket(const DiskLoc& bucket);

        virtual Status setOptions(const CursorOptions& options);

        virtual Status seek(const BSONObj& position);

        virtual Status seek(const vector<const BSONElement*>& position,
                            const vector<bool>& inclusive);

        virtual Status skip(const vector<const BSONElement*>& position,
                            const vector<bool>& inclusive);

        virtual BSONObj getKey() const;
        virtual DiskLoc getValue() const;
        virtual void next();

        virtual Status savePosition();

        virtual Status restorePosition();

        virtual string toString();

    private:
        // We keep the constructor private and only allow the AM to create us.
        friend class BtreeAccessMethod;

        // Go forward by default.
        BtreeIndexCursor(IndexDescriptor *descriptor, Ordering ordering, BtreeInterface *interface);

        void skipUnusedKeys();

        bool isSavedPositionValid();

        // Move to the next/prev. key.  Used by normal getNext and also skipping unused keys.
        void advance(const char* caller);

        // For saving/restoring position.
        BSONObj _savedKey;
        DiskLoc _savedLoc;

        BSONObj _emptyObj;

        int _direction;
        IndexDescriptor* _descriptor;
        Ordering _ordering;
        BtreeInterface* _interface;

        // What are we looking at RIGHT NOW?  We look at a bucket.
        DiskLoc _bucket;
        // And we look at an offset in the bucket.
        int _keyOffset;
    };

}  // namespace mongo
