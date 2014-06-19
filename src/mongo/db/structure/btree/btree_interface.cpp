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

#include "mongo/db/structure/btree/btree_interface.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/structure/btree/btree_logic.h"


namespace mongo {

    template <class OnDiskFormat>
    class BtreeBuilderInterfaceImpl : public BtreeBuilderInterface {
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
    class BtreeInterfaceImpl : public BtreeInterface {
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

        virtual BtreeBuilderInterface* getBulkBuilder(OperationContext* txn,
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

        virtual void fullValidate(long long *numKeysOut) {
            *numKeysOut = _btree->fullValidate(NULL, false, false, 0);
        }

        virtual Status dupKeyCheck(const BSONObj& key, const DiskLoc& loc) {
            return _btree->dupKeyCheck(key, loc);
        }

        virtual bool isEmpty() {
            return _btree->isEmpty();
        }

        virtual Status touch(OperationContext* txn) const{
            return _btree->touch(txn);
        }

        class Cursor : public BtreeInterface::Cursor {
        public:
            Cursor(const BtreeLogic<OnDiskFormat>* btree, int direction)
                : _btree(btree),
                  _direction(direction),
                  _bucket(btree->getHead()), // XXX this shouldn't be nessisary, but is.
                  _ofs(0) {
            }

            virtual int getDirection() const { return _direction; }

            virtual bool isEOF() const { return _bucket.isNull(); }

            virtual bool pointsToSamePlaceAs(const BtreeInterface::Cursor& otherBase) const {
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
                return _btree->locate(key, loc, _direction, &_ofs, &_bucket);
            }

            virtual void customLocate(const BSONObj& keyBegin,
                                      int keyBeginLen,
                                      bool afterKey,
                                      const vector<const BSONElement*>& keyEnd,
                                      const vector<bool>& keyEndInclusive) {

                _btree->customLocate(&_bucket,
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

                _btree->advanceTo(&_bucket,
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
                _btree->advance(&_bucket, &_ofs, _direction);
            }

            virtual void savePosition(SavedPositionData* savedOut) const {
                savedOut->key = getKey().getOwned();
                savedOut->loc = getDiskLoc();
            }

            virtual void restorePosition(const SavedPositionData& saved) {
                _btree->restorePosition(saved.key,
                                        saved.loc,
                                        _direction,
                                        &_bucket,
                                        &_ofs);
            }

        private:
            const BtreeLogic<OnDiskFormat>* const _btree;
            const int _direction;

            DiskLoc _bucket;
            int _ofs;
        };

        virtual Cursor* newCursor(int direction) const {
            return new Cursor(_btree.get(), direction);
        }

        virtual Status initAsEmpty(OperationContext* txn) {
            return _btree->initAsEmpty(txn);
        }

    private:
        scoped_ptr<BtreeLogic<OnDiskFormat> > _btree;
    };

    // static
    BtreeInterface* BtreeInterface::getInterface(HeadManager* headManager,
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
