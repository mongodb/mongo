/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/storage/sorted_data_interface.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/btree/btree_logic.h"


namespace mongo {

    template <class OnDiskFormat>
    class BtreeBuilderInterfaceImpl : public SortedDataBuilderInterface {
    public:
        BtreeBuilderInterfaceImpl(OperationContext* trans,
                                  typename BtreeLogic<OnDiskFormat>::Builder* builder)
            : _builder(builder), _trans(trans) { }

        virtual ~BtreeBuilderInterfaceImpl() { }

        Status addKey(const BSONObj& key, const DiskLoc& loc) {
            return _builder->addKey(key, loc);
        }

        unsigned long long commit(bool mayInterrupt) {
            return _builder->commit(mayInterrupt);
        }

    private:
        typename BtreeLogic<OnDiskFormat>::Builder* _builder;

        // Not owned here.
        OperationContext* _trans;
    };

    template <class OnDiskFormat>
    class BtreeInterfaceImpl : public SortedDataInterface {
    public:
        BtreeInterfaceImpl(HeadManager* headManager,
                                RecordStore* recordStore,
                                const Ordering& ordering,
                                const string& indexName,
                                BucketDeletionNotification* bucketDeletionNotification) {

            _btree.reset(new BtreeLogic<OnDiskFormat>(headManager,
                                                      recordStore,
                                                      ordering,
                                                      indexName,
                                                      bucketDeletionNotification));
        }

        virtual ~BtreeInterfaceImpl() { }

        virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn,
                                                           bool dupsAllowed) {

            return new BtreeBuilderInterfaceImpl<OnDiskFormat>(
                txn, _btree->newBuilder(txn, dupsAllowed));
        }

        virtual Status insert(OperationContext* txn,
                              const BSONObj& key,
                              const DiskLoc& loc,
                              bool dupsAllowed) {

            return _btree->insert(txn, key, loc, dupsAllowed);
        }

        virtual bool unindex(OperationContext* txn,
                             const BSONObj& key,
                             const DiskLoc& loc) {

            return _btree->unindex(txn, key, loc);
        }

        virtual void fullValidate(OperationContext* txn, long long *numKeysOut) {
            *numKeysOut = _btree->fullValidate(txn, NULL, false, false, 0);
        }

        virtual Status dupKeyCheck(OperationContext* txn,
                                   const BSONObj& key,
                                   const DiskLoc& loc) {
            return _btree->dupKeyCheck(txn, key, loc);
        }

        virtual bool isEmpty() {
            return _btree->isEmpty();
        }

        virtual Status touch(OperationContext* txn) const{
            return _btree->touch(txn);
        }

        class Cursor : public SortedDataInterface::Cursor {
        public:
            Cursor(OperationContext* txn,
                   const BtreeLogic<OnDiskFormat>* btree,
                   int direction)
                : _txn(txn),
                  _btree(btree),
                  _direction(direction),
                  _bucket(btree->getHead()), // XXX this shouldn't be nessisary, but is.
                  _ofs(0) {
            }

            virtual int getDirection() const { return _direction; }

            virtual bool isEOF() const { return _bucket.isNull(); }

            virtual bool pointsToSamePlaceAs(const SortedDataInterface::Cursor& otherBase) const {
                const Cursor& other = static_cast<const Cursor&>(otherBase);
                if (isEOF())
                    return other.isEOF();

                return _bucket == other._bucket && _ofs == other._ofs;

            }

            virtual void aboutToDeleteBucket(const DiskLoc& bucket) {
                if (_bucket == bucket)
                    _ofs = -1;
            }

            virtual bool locate(const BSONObj& key, const DiskLoc& loc) {
                return _btree->locate(_txn, key, loc, _direction, &_ofs, &_bucket);
            }

            virtual void customLocate(const BSONObj& keyBegin,
                                      int keyBeginLen,
                                      bool afterKey,
                                      const vector<const BSONElement*>& keyEnd,
                                      const vector<bool>& keyEndInclusive) {

                _btree->customLocate(_txn,
                                     &_bucket,
                                     &_ofs,
                                     keyBegin,
                                     keyBeginLen,
                                     afterKey, 
                                     keyEnd,
                                     keyEndInclusive,
                                     _direction);
            }

            void advanceTo(const BSONObj &keyBegin,
                           int keyBeginLen,
                           bool afterKey,
                           const vector<const BSONElement*>& keyEnd,
                           const vector<bool>& keyEndInclusive) {

                _btree->advanceTo(_txn,
                                  &_bucket,
                                  &_ofs,
                                  keyBegin,
                                  keyBeginLen,
                                  afterKey,
                                  keyEnd,
                                  keyEndInclusive,
                                  _direction);
            }

            virtual BSONObj getKey() const {
                return _btree->getKey(_bucket, _ofs);
            }

            virtual DiskLoc getDiskLoc() const {
                return _btree->getDiskLoc(_bucket, _ofs);
            }

            virtual void advance() {
                _btree->advance(_txn, &_bucket, &_ofs, _direction);
            }

            virtual void savePosition() {
                if (!_bucket.isNull()) {
                    _savedKey = getKey().getOwned();
                    _savedLoc = getDiskLoc();
                }
            }

            virtual void restorePosition() {
                if (!_bucket.isNull()) {
                    _btree->restorePosition(_txn,
                                            _savedKey,
                                            _savedLoc,
                                            _direction,
                                            &_bucket,
                                            &_ofs);
                }
            }

        private:
            OperationContext* _txn; // not owned
            const BtreeLogic<OnDiskFormat>* const _btree;
            const int _direction;

            DiskLoc _bucket;
            int _ofs;

            // Only used by save/restorePosition() if _bucket is non-Null.
            BSONObj _savedKey;
            DiskLoc _savedLoc;
        };

        virtual Cursor* newCursor(OperationContext* txn, int direction) const {
            return new Cursor(txn, _btree.get(), direction);
        }

        virtual Status initAsEmpty(OperationContext* txn) {
            return _btree->initAsEmpty(txn);
        }

    private:
        scoped_ptr<BtreeLogic<OnDiskFormat> > _btree;
    };

    SortedDataInterface* getMMAPV1Interface(HeadManager* headManager,
                                            RecordStore* recordStore,
                                            const Ordering& ordering,
                                            const string& indexName,
                                            int version,
                                            BucketDeletionNotification* bucketDeletion) {

        if (0 == version) {
            return new BtreeInterfaceImpl<BtreeLayoutV0>(headManager,
                                                         recordStore,
                                                         ordering,
                                                         indexName,
                                                         bucketDeletion);
        }
        else {
            invariant(1 == version);
            return new BtreeInterfaceImpl<BtreeLayoutV1>(headManager,
                                                         recordStore,
                                                         ordering,
                                                         indexName,
                                                         bucketDeletion);
        }
    }

}  // namespace mongo
