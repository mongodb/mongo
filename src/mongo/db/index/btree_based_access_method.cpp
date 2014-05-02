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

#include "mongo/db/index/btree_access_method.h"

#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/curop.h"
#include "mongo/db/extsort.h"
#include "mongo/db/index/btree_based_bulk_access_method.h"
#include "mongo/db/index/btree_index_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/transaction.h"
#include "mongo/db/structure/btree/btree_interface.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

    MONGO_EXPORT_SERVER_PARAMETER(failIndexKeyTooLong, bool, true);

    BtreeBasedAccessMethod::BtreeBasedAccessMethod(IndexCatalogEntry* btreeState)
        : _btreeState(btreeState), _descriptor(btreeState->descriptor()) {

        verify(0 == _descriptor->version() || 1 == _descriptor->version());
        _newInterface.reset(BtreeInterface::getInterface(btreeState->headManager(),
                                                         btreeState->recordStore(),
                                                         btreeState->ordering(),
                                                         _descriptor->indexNamespace(),
                                                         _descriptor->version()));
    }

    // Find the keys for obj, put them in the tree pointing to loc
    Status BtreeBasedAccessMethod::insert(TransactionExperiment* txn,
                                          const BSONObj& obj,
                                          const DiskLoc& loc,
                                          const InsertDeleteOptions& options,
                                          int64_t* numInserted) {
        *numInserted = 0;

        BSONObjSet keys;
        // Delegate to the subclass.
        getKeys(obj, &keys);

        Status ret = Status::OK();
        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            Status status = _newInterface->insert(txn, *i, loc, options.dupsAllowed);

            // Everything's OK, carry on.
            if (status.isOK()) {
                ++*numInserted;
                continue;
            }

            // Error cases.

            if (ErrorCodes::KeyTooLong == status.code()) {
                // Ignore this error if we're on a secondary.
                if (!isMasterNs(collection()->ns().ns().c_str())) {
                    continue;
                }

                // The user set a parameter to ignore key too long errors.
                if (!failIndexKeyTooLong) {
                    continue;
                }
            }

            if (ErrorCodes::UniqueIndexViolation == status.code()) {
                // We ignore it for some reason in BG indexing.
                if (!_btreeState->isReady()) {
                    DEV log() << "info: key already in index during bg indexing (ok)\n";
                    continue;
                }
            }

            // Clean up after ourselves.
            for (BSONObjSet::const_iterator j = keys.begin(); j != i; ++j) {
                removeOneKey(txn, *j, loc);
                *numInserted = 0;
            }

            return status;
        }

        if (*numInserted > 1) {
            // XXX: this should use a txn?
            _btreeState->setMultikey();
        }

        return ret;
    }

    bool BtreeBasedAccessMethod::removeOneKey(TransactionExperiment* txn,
                                              const BSONObj& key,
                                              const DiskLoc& loc) {
        bool ret = false;

        try {
            ret = _newInterface->unindex(txn, key, loc);
        } catch (AssertionException& e) {
            problem() << "Assertion failure: _unindex failed "
                << _descriptor->indexNamespace() << endl;
            out() << "Assertion failure: _unindex failed: " << e.what() << '\n';
            out() << "  obj:" << _btreeState->collection()->docFor(loc).toString() << '\n';
            out() << "  key:" << key.toString() << '\n';
            out() << "  dl:" << loc.toString() << endl;
            logContext();
        }

        return ret;
    }

    Status BtreeBasedAccessMethod::newCursor(IndexCursor **out) const {
        *out = new BtreeIndexCursor(_btreeState->head(),
                                    _newInterface.get());
        return Status::OK();
    }

    // Remove the provided doc from the index.
    Status BtreeBasedAccessMethod::remove(TransactionExperiment* txn,
                                          const BSONObj &obj,
                                          const DiskLoc& loc,
                                          const InsertDeleteOptions &options,
                                          int64_t* numDeleted) {

        BSONObjSet keys;
        getKeys(obj, &keys);
        *numDeleted = 0;

        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            bool thisKeyOK = removeOneKey(txn, *i, loc);

            if (thisKeyOK) {
                ++*numDeleted;
            } else if (options.logIfError) {
                log() << "unindex failed (key too big?) " << _descriptor->indexNamespace()
                      << " key: " << *i << " " 
                      << _btreeState->collection()->docFor(loc)["_id"] << endl;
            }
        }

        return Status::OK();
    }

    // Return keys in l that are not in r.
    // Lifted basically verbatim from elsewhere.
    static void setDifference(const BSONObjSet &l, const BSONObjSet &r, vector<BSONObj*> *diff) {
        // l and r must use the same ordering spec.
        verify(l.key_comp().order() == r.key_comp().order());
        BSONObjSet::const_iterator i = l.begin();
        BSONObjSet::const_iterator j = r.begin();
        while ( 1 ) {
            if ( i == l.end() )
                break;
            while ( j != r.end() && j->woCompare( *i ) < 0 )
                j++;
            if ( j == r.end() || i->woCompare(*j) != 0  ) {
                const BSONObj *jo = &*i;
                diff->push_back( (BSONObj *) jo );
            }
            i++;
        }
    }

    Status BtreeBasedAccessMethod::initializeAsEmpty(TransactionExperiment* txn) {
        return _newInterface->initAsEmpty(txn);
    }

    Status BtreeBasedAccessMethod::touch(const BSONObj& obj) {
        BSONObjSet keys;
        getKeys(obj, &keys);

        DiskLoc loc;
        int keyPos;
        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            _newInterface->locate(*i, DiskLoc(), 1, &loc, &keyPos);
        }

        return Status::OK();
    }

    Status BtreeBasedAccessMethod::touch( TransactionExperiment* txn ) const {
        return _btreeState->recordStore()->touch( txn, NULL );
    }

    DiskLoc BtreeBasedAccessMethod::findSingle(const BSONObj& key) const {
        DiskLoc bucket;
        int pos;

        _newInterface->locate(key, minDiskLoc, 1, &bucket, &pos);

        // A null bucket means the key wasn't found (nor was anything found after it).
        if (bucket.isNull()) {
            return DiskLoc();
        }

        // We found something but it could be a key after 'key'.  Examine what we're pointing at.
        if (0 != key.woCompare(_newInterface->getKey(bucket, pos), BSONObj(), false)) {
            // If the keys don't match, return "not found."
            return DiskLoc();
        }

        // Return the DiskLoc found.
        return _newInterface->getDiskLoc(bucket, pos);
    }

    Status BtreeBasedAccessMethod::validate(int64_t* numKeys) {
        // XXX: long long vs int64_t
        long long keys;
        _newInterface->fullValidate(&keys);
        *numKeys = keys;
        return Status::OK();
    }

    Status BtreeBasedAccessMethod::validateUpdate(const BSONObj &from,
                                                  const BSONObj &to,
                                                  const DiskLoc &record,
                                                  const InsertDeleteOptions &options,
                                                  UpdateTicket* status) {

        BtreeBasedPrivateUpdateData *data = new BtreeBasedPrivateUpdateData();
        status->_indexSpecificUpdateData.reset(data);

        getKeys(from, &data->oldKeys);
        getKeys(to, &data->newKeys);
        data->loc = record;
        data->dupsAllowed = options.dupsAllowed;

        setDifference(data->oldKeys, data->newKeys, &data->removed);
        setDifference(data->newKeys, data->oldKeys, &data->added);

        bool checkForDups = !data->added.empty()
            && (KeyPattern::isIdKeyPattern(_descriptor->keyPattern()) || _descriptor->unique())
            && !options.dupsAllowed;

        if (checkForDups) {
            for (vector<BSONObj*>::iterator i = data->added.begin(); i != data->added.end(); i++) {
                Status check = _newInterface->dupKeyCheck(**i, record);
                if (!check.isOK()) {
                    status->_isValid = false;
                    return check;
                }
            }
        }

        status->_isValid = true;

        return Status::OK();
    }

    Status BtreeBasedAccessMethod::update(TransactionExperiment* txn,
                                          const UpdateTicket& ticket,
                                          int64_t* numUpdated) {
        if (!ticket._isValid) {
            return Status(ErrorCodes::InternalError, "Invalid UpdateTicket in update");
        }

        BtreeBasedPrivateUpdateData* data =
            static_cast<BtreeBasedPrivateUpdateData*>(ticket._indexSpecificUpdateData.get());

        if (data->oldKeys.size() + data->added.size() - data->removed.size() > 1) {
            _btreeState->setMultikey();
        }

        for (size_t i = 0; i < data->added.size(); ++i) {
            _newInterface->insert(txn, *data->added[i], data->loc, data->dupsAllowed);
        }

        for (size_t i = 0; i < data->removed.size(); ++i) {
            _newInterface->unindex(txn, *data->removed[i], data->loc);
        }

        *numUpdated = data->added.size();

        return Status::OK();
    }

    IndexAccessMethod* BtreeBasedAccessMethod::initiateBulk(TransactionExperiment* txn) {
        // If there's already data in the index, don't do anything.
        if (!_newInterface->isEmpty()) {
            return NULL;
        }

        return new BtreeBasedBulkAccessMethod(txn,
                                              this,
                                              _newInterface.get(),
                                              _descriptor,
                                              _btreeState->collection()->numRecords());
    }

    Status BtreeBasedAccessMethod::commitBulk(IndexAccessMethod* bulkRaw,
                                              bool mayInterrupt,
                                              set<DiskLoc>* dupsToDrop) {
        if (!_newInterface->isEmpty()) {
            return Status(ErrorCodes::InternalError, "trying to commit but has data already");
        }

        BtreeBasedBulkAccessMethod* bulk = static_cast<BtreeBasedBulkAccessMethod*>(bulkRaw);
        return bulk->commit(dupsToDrop, cc().curop(), mayInterrupt);
    }

}  // namespace mongo
