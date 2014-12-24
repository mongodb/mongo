/**
*    Copyright (C) 2013-2014 MongoDB Inc.
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

#include <boost/scoped_ptr.hpp>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

    class ExternalSortComparison;

    /**
     * Any access method that is Btree based subclasses from this.
     *
     * Subclassers must:
     * 1. Call the constructor for this class from their constructors, and
     * 2. override getKeys.
     *
     * XXX: Should really think of the sub-class as providing an expression mapping of the input,
     * don't need so many AMs, just really precomputing some data and mapping doc for getKeys(?).
     * See SERVER-12397 for tracking.
     */
    class BtreeBasedAccessMethod : public IndexAccessMethod {
        MONGO_DISALLOW_COPYING( BtreeBasedAccessMethod );
    public:
        BtreeBasedAccessMethod( IndexCatalogEntry* btreeState,
                                SortedDataInterface* btree );

        virtual ~BtreeBasedAccessMethod() { }

        virtual Status insert(OperationContext* txn,
                              const BSONObj& obj,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numInserted);

        virtual Status remove(OperationContext* txn,
                              const BSONObj& obj,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numDeleted);

        virtual Status validateUpdate(OperationContext* txn,
                                      const BSONObj& from,
                                      const BSONObj& to,
                                      const RecordId& loc,
                                      const InsertDeleteOptions& options,
                                      UpdateTicket* ticket);

        virtual Status update(OperationContext* txn,
                              const UpdateTicket& ticket,
                              int64_t* numUpdated);

        virtual Status newCursor(OperationContext* txn,
                                 const CursorOptions& opts,
                                 IndexCursor** out) const;

        virtual Status initializeAsEmpty(OperationContext* txn);

        virtual IndexAccessMethod* initiateBulk(OperationContext* txn);

        virtual Status commitBulk( IndexAccessMethod* bulk,
                                   bool mayInterrupt,
                                   bool dupsAllowed,
                                   std::set<RecordId>* dups );

        virtual Status touch(OperationContext* txn, const BSONObj& obj);

        virtual Status touch(OperationContext* txn) const;

        virtual Status validate(OperationContext* txn, bool full, int64_t* numKeys,
                                BSONObjBuilder* output);

        virtual bool appendCustomStats(OperationContext* txn, BSONObjBuilder* output, double scale)
            const;
        virtual long long getSpaceUsedBytes( OperationContext* txn ) const;

        // XXX: consider migrating callers to use IndexCursor instead
        virtual RecordId findSingle( OperationContext* txn, const BSONObj& key ) const;

    protected:
        // Friends who need getKeys.
        friend class BtreeBasedBulkAccessMethod;

        // See below for body.
        class BtreeBasedPrivateUpdateData;

        virtual void getKeys(const BSONObj &obj, BSONObjSet *keys) = 0;

        // Determines whether it's OK to ignore ErrorCodes::KeyTooLong for this OperationContext
        bool ignoreKeyTooLong(OperationContext* txn);

        IndexCatalogEntry* _btreeState; // owned by IndexCatalogEntry
        const IndexDescriptor* _descriptor;

    private:
        void removeOneKey(OperationContext* txn,
                          const BSONObj& key,
                          const RecordId& loc,
                          bool dupsAllowed);

        boost::scoped_ptr<SortedDataInterface> _newInterface;
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
        std::vector<BSONObj*> removed, added;

        RecordId loc;
        bool dupsAllowed;
    };

}  // namespace mongo
