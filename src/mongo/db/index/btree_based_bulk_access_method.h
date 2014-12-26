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

#include <set>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/index/btree_based_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

    class BtreeBasedBulkAccessMethod : public IndexAccessMethod {
    public:
        /**
         * Does not take ownership of any pointers.
         * All pointers must outlive 'this'.
         */
        BtreeBasedBulkAccessMethod(OperationContext* txn,
                                   BtreeBasedAccessMethod* real,
                                   SortedDataInterface* interface,
                                   const IndexDescriptor* descriptor);

        ~BtreeBasedBulkAccessMethod() {}

        virtual Status insert(OperationContext* txn,
                              const BSONObj& obj,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numInserted);

        Status commit(std::set<RecordId>* dupsToDrop, bool mayInterrupt, bool dupsAllowed);

        // Exposed for testing.
        static ExternalSortComparison* getComparison(int version, const BSONObj& keyPattern);

        //
        // Stuff below here is a no-op of one form or another.
        //

        virtual Status commitBulk(IndexAccessMethod* bulk,
                                  bool mayInterrupt,
                                  bool dupsAllowed,
                                  std::set<RecordId>* dups) {
            invariant(this == bulk);
            return Status::OK();
        }

        virtual Status touch(OperationContext* txn, const BSONObj& obj) {
            return _notAllowed();
        }

        virtual Status touch(OperationContext* txn) const {
            return _notAllowed();
        }

        virtual Status validate(OperationContext* txn, bool full, int64_t* numKeys, BSONObjBuilder* output) {
            return _notAllowed();
        }

        virtual bool appendCustomStats(OperationContext* txn, BSONObjBuilder* output, double scale)
            const {
            return false;
        }

        virtual Status remove(OperationContext* txn,
                              const BSONObj& obj,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numDeleted) {
            return _notAllowed();
        }

        virtual Status validateUpdate(OperationContext* txn,
                                      const BSONObj& from,
                                      const BSONObj& to,
                                      const RecordId& loc,
                                      const InsertDeleteOptions& options,
                                      UpdateTicket* ticket) {
            return _notAllowed();
        }

        virtual long long getSpaceUsedBytes( OperationContext* txn ) const {
            return -1;
        }

        virtual Status update(OperationContext* txn,
                              const UpdateTicket& ticket,
                              int64_t* numUpdated) {
            return _notAllowed();
        }

        virtual Status newCursor(OperationContext*txn,
                                 const CursorOptions& opts,
                                 IndexCursor** out) const {
            return _notAllowed();
        }

        virtual Status initializeAsEmpty(OperationContext* txn) {
            return _notAllowed();
        }

        virtual IndexAccessMethod* initiateBulk(OperationContext* txn) {
            return NULL;
        }

        OperationContext* getOperationContext() { return _txn; }

    private:
        typedef Sorter<BSONObj, RecordId> BSONObjExternalSorter;

        Status _notAllowed() const {
            return Status(ErrorCodes::InternalError, "cannot use bulk for this yet");
        }

        // Not owned here.
        BtreeBasedAccessMethod* _real;

        // Not owned here.
        SortedDataInterface* _interface;

        // The external sorter.
        boost::scoped_ptr<BSONObjExternalSorter> _sorter;

        // How many docs are we indexing?
        unsigned long long _docsInserted;

        // And how many keys?
        unsigned long long _keysInserted;

        // Does any document have >1 key?
        bool _isMultiKey;

        OperationContext* _txn;
    };

}  // namespace mongo
