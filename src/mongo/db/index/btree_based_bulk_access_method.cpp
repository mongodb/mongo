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

#include "mongo/db/index/btree_based_bulk_access_method.h"

#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile_private.h"  // This is for inDBRepair.
#include "mongo/db/repl/rs.h"         // This is for ignoreUniqueIndex.
#include "mongo/db/operation_context.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

    //
    // Comparison for external sorter interface
    //

    // Defined in db/structure/btree/key.cpp
    // XXX TODO: rename to something more descriptive, etc. etc.
    int oldCompare(const BSONObj& l,const BSONObj& r, const Ordering &o);

    class BtreeExternalSortComparisonV0 : public ExternalSortComparison {
    public:
        BtreeExternalSortComparisonV0(const BSONObj& ordering)
            : _ordering(Ordering::make(ordering)){
        }

        virtual ~BtreeExternalSortComparisonV0() { }

        virtual int compare(const ExternalSortDatum& l, const ExternalSortDatum& r) const {
            int x = oldCompare(l.first, r.first, _ordering);
            if (x) { return x; }
            return l.second.compare(r.second);
        }
    private:
        const Ordering _ordering;
    };

    class BtreeExternalSortComparisonV1 : public ExternalSortComparison {
    public:
        BtreeExternalSortComparisonV1(const BSONObj& ordering)
            : _ordering(Ordering::make(ordering)) {
        }

        virtual ~BtreeExternalSortComparisonV1() { }

        virtual int compare(const ExternalSortDatum& l, const ExternalSortDatum& r) const {
            int x = l.first.woCompare(r.first, _ordering, /*considerfieldname*/false);
            if (x) { return x; }
            return l.second.compare(r.second);
        }
    private:
        const Ordering _ordering;
    };

    // static
    ExternalSortComparison* BtreeBasedBulkAccessMethod::getComparison(int version, const BSONObj& keyPattern) {
        if (0 == version) {
            return new BtreeExternalSortComparisonV0(keyPattern);
        }
        else if (1 == version) {
            return new BtreeExternalSortComparisonV1(keyPattern);
        }
        verify( 0 );
        return NULL;
    }

    BtreeBasedBulkAccessMethod::BtreeBasedBulkAccessMethod(OperationContext* txn,
                                                           BtreeBasedAccessMethod* real,
                                                           BtreeInterface* interface,
                                                           const IndexDescriptor* descriptor,
                                                           int numRecords) {
        _real = real;
        _interface = interface;
        _txn = txn;

        _docsInserted = 0;
        _keysInserted = 0;
        _isMultiKey = false;

        _sortCmp.reset(getComparison(descriptor->version(), descriptor->keyPattern()));
        _sorter.reset(new BSONObjExternalSorter(_sortCmp.get()));
        _sorter->hintNumObjects(numRecords);
    }

    Status BtreeBasedBulkAccessMethod::insert(OperationContext* txn,
                                              const BSONObj& obj,
                                              const DiskLoc& loc,
                                              const InsertDeleteOptions& options,
                                              int64_t* numInserted) {
        BSONObjSet keys;
        _real->getKeys(obj, &keys);

        _isMultiKey = _isMultiKey || (keys.size() > 1);

        for (BSONObjSet::iterator it = keys.begin(); it != keys.end(); ++it) {
            // False is for mayInterrupt.
            _sorter->add(*it, loc, false);
            _keysInserted++;
        }

        _docsInserted++;

        if (NULL != numInserted) {
            *numInserted += keys.size();
        }

        return Status::OK();
    }

    Status BtreeBasedBulkAccessMethod::commit(set<DiskLoc>* dupsToDrop,
                                              CurOp* op,
                                              bool mayInterrupt) {
        DiskLoc oldHead = _real->_btreeState->head();

        // XXX: do we expect the tree to be empty but have a head set?  Looks like so from old code.
        invariant(!oldHead.isNull());
        _real->_btreeState->setHead(_txn, DiskLoc());
        _real->_btreeState->recordStore()->deleteRecord(_txn, oldHead);

        if (_isMultiKey) {
            _real->_btreeState->setMultikey( _txn );
        }

        _sorter->sort(false);

        Timer timer;
        IndexCatalogEntry* entry = _real->_btreeState;

        bool dupsAllowed = !entry->descriptor()->unique()
                           || ignoreUniqueIndex(entry->descriptor());

        bool dropDups = entry->descriptor()->dropDups() || inDBRepair;

        scoped_ptr<BSONObjExternalSorter::Iterator> i(_sorter->iterator());

        // verifies that pm and op refer to the same ProgressMeter
        ProgressMeter& pm = op->setMessage("Index Bulk Build: (2/3) btree bottom up",
                                           "Index: (2/3) BTree Bottom Up Progress",
                                           _keysInserted,
                                           10);

        scoped_ptr<BtreeBuilderInterface> builder;

        builder.reset(_interface->getBulkBuilder(_txn, dupsAllowed));

        while (i->more()) {
            // Get the next datum and add it to the builder.
            ExternalSortDatum d = i->next();
            Status status = builder->addKey(d.first, d.second);

            if (!status.isOK()) {
                if (ErrorCodes::DuplicateKey != status.code()) {
                    return status;
                }

                // If we're here it's a duplicate key.
                if (dropDups) {
                    static const size_t kMaxDupsToStore = 1000000;
                    dupsToDrop->insert(d.second);
                    if (dupsToDrop->size() > kMaxDupsToStore) {
                        return Status(ErrorCodes::InternalError,
                                      "Too many dups on index build with dropDups = true");
                    }
                }
                else if (!dupsAllowed) {
                    return status;
                }
            }

            // If we're here either it's a dup and we're cool with it or the addKey went just
            // fine.
            pm.hit();
        }

        pm.finished();

        op->setMessage("Index Bulk Build: (3/3) btree-middle",
                       "Index: (3/3) BTree Middle Progress");

        LOG(timer.seconds() > 10 ? 0 : 1 ) << "\t done building bottom layer, going to commit";

        unsigned long long keysCommit = builder->commit(mayInterrupt);

        if (!dropDups && (keysCommit != _keysInserted)) {
            warning() << "not all entries were added to the index, probably some "
                      << "keys were too large" << endl;
        }
        return Status::OK();
    }

}  // namespace mongo
