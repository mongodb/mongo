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

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/index/btree_interface.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

    /**
     * Any access method that is Btree based subclasses from this.
     *
     * Subclassers must:
     * 1. Call the constructor for this class from their constructors,
     * 2. override newCursor, and
     * 3. override getKeys.
     */
    class BtreeBasedAccessMethod : public IndexAccessMethod {
    public:
        BtreeBasedAccessMethod(IndexDescriptor *descriptor);
        virtual ~BtreeBasedAccessMethod() { }

        virtual Status insert(const BSONObj& obj,
                              const DiskLoc& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numInserted);

        virtual Status remove(const BSONObj& obj,
                              const DiskLoc& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numDeleted);

        virtual Status validateUpdate(const BSONObj& from,
                                      const BSONObj& to,
                                      const DiskLoc& loc,
                                      const InsertDeleteOptions& options,
                                      UpdateTicket* ticket);

        virtual Status update(const UpdateTicket& ticket);

        virtual Status newCursor(IndexCursor **out) = 0;

        virtual Status touch(const BSONObj& obj);

    protected:
        // Friends who need getKeys.
        // TODO: uncomment when builder is in.
        // template <class K> friend class BtreeBasedIndexBuilder;

        // See below for body.
        class BtreeBasedPrivateUpdateData;

        virtual void getKeys(const BSONObj &obj, BSONObjSet *keys) = 0;

        IndexDescriptor* _descriptor;
        Ordering _ordering;

        // There are 2 types of Btree disk formats.  We put them both behind one interface.
        BtreeInterface* _interface;

    private:
        bool removeOneKey(const BSONObj& key, const DiskLoc& loc);
    };

    /**
     * What data do we need to perform an update?
     */
    class BtreeBasedAccessMethod::BtreeBasedPrivateUpdateData
        : public UpdateTicket::PrivateUpdateData {
    public:
        virtual ~BtreeBasedPrivateUpdateData() { }

        BSONObjSet oldKeys, newKeys;

        // These point into the sets oldKeys and newKeys.
        vector<BSONObj*> removed, added;

        DiskLoc loc;
        bool dupsAllowed;
    };

}  // namespace mongo
