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

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/btree_interface.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/structure/btree/btree.h"
#include "mongo/db/structure/record_store.h"

namespace mongo {

    template <class Version>
    class BtreeInterfaceImpl : public BtreeInterface {
    public:
        // typedef typename BucketBasics<Version>::VersionNode VersionNode;

        virtual ~BtreeInterfaceImpl() { }

        const BtreeBucket<Version>* getHeadBucket( const IndexCatalogEntry* entry ) const {
            return getBucket( entry->head() );
        }

        const BtreeBucket<Version>* getBucket( const IndexCatalogEntry* entry,
                                               const DiskLoc& loc ) const {
            Record* record = entry->recordStore()->recordFor( loc );
            return BtreeBucket<Version>::asVersion( record );
        }

        virtual int bt_insert(IndexCatalogEntry* btreeState,
                              const DiskLoc thisLoc,
                              const DiskLoc recordLoc,
                              const BSONObj& key,
                              bool dupsallowed,
                              bool toplevel) {
            // FYI: toplevel has a default value of true in btree.h
            return getBucket( btreeState, thisLoc )->bt_insert(btreeState,
                                                                      thisLoc,
                                                                      recordLoc,
                                                                      key,
                                                                      dupsallowed,
                                                                      toplevel);
        }

        virtual bool unindex(IndexCatalogEntry* btreeState,
                             const DiskLoc thisLoc,
                             const BSONObj& key,
                             const DiskLoc recordLoc) {
            return getBucket( btreeState, thisLoc )->unindex(btreeState,
                                                                    thisLoc,
                                                                    key,
                                                                    recordLoc);
        }

        virtual DiskLoc locate(const IndexCatalogEntry* btreeState,
                               const DiskLoc& thisLoc,
                               const BSONObj& key,
                               int& pos,
                               bool& found,
                               const DiskLoc& recordLoc,
                               int direction) const {
            // FYI: direction has a default of 1
            return getBucket( btreeState, thisLoc )->locate(btreeState,
                                                                   thisLoc,
                                                                   key,
                                                                   pos,
                                                                   found,
                                                                   recordLoc,
                                                                   direction);
        }

        virtual bool wouldCreateDup(const IndexCatalogEntry* btreeState,
                                    const DiskLoc& thisLoc,
                                    const BSONObj& key,
                                    const DiskLoc& self) const {
            typename Version::KeyOwned ownedVersion(key);
            return getBucket( btreeState, thisLoc )->wouldCreateDup(btreeState,
                                                                           thisLoc,
                                                                           ownedVersion,
                                                                           self);
        }

        virtual void customLocate(const IndexCatalogEntry* btreeState,
                                  DiskLoc& locInOut,
                                  int& keyOfs,
                                  const BSONObj& keyBegin,
                                  int keyBeginLen, bool afterVersion,
                                  const vector<const BSONElement*>& keyEnd,
                                  const vector<bool>& keyEndInclusive,
                                  int direction,
                                  pair<DiskLoc, int>& bestParent) const {
            return BtreeBucket<Version>::customLocate(btreeState,
                                                      locInOut,
                                                      keyOfs,
                                                      keyBegin,
                                                      keyBeginLen, afterVersion,
                                                      keyEnd,
                                                      keyEndInclusive,
                                                      direction,
                                                      bestParent);
        }

        virtual void advanceTo(const IndexCatalogEntry* btreeState,
                               DiskLoc &thisLoc,
                               int &keyOfs,
                               const BSONObj &keyBegin,
                               int keyBeginLen,
                               bool afterVersion,
                               const vector<const BSONElement*>& keyEnd,
                               const vector<bool>& keyEndInclusive,
                               int direction) const {
            return getBucket( btreeState, thisLoc )->advanceTo(btreeState,
                                                                      thisLoc,
                                                                      keyOfs,
                                                                      keyBegin,
                                                                      keyBeginLen,
                                                                      afterVersion,
                                                                      keyEnd,
                                                                      keyEndInclusive,
                                                                      direction);
        }


        virtual bool keyIsUsed(const IndexCatalogEntry* btreeState,
                               DiskLoc bucket, int keyOffset) const {
            return getBucket(btreeState,bucket)->k(keyOffset).isUsed();
        }

        virtual BSONObj keyAt(const IndexCatalogEntry* btreeState,
                              DiskLoc bucket, int keyOffset) const {
            verify(!bucket.isNull());
            const BtreeBucket<Version> *b = getBucket(btreeState,bucket);
            int n = b->getN();
            if (n == b->INVALID_N_SENTINEL) {
                throw UserException(deletedBucketCode, "keyAt bucket deleted");
            }
            dassert( n >= 0 && n < 10000 );
            return keyOffset >= n ? BSONObj() : b->keyNode(keyOffset).key.toBson();
        }

        virtual DiskLoc recordAt(const IndexCatalogEntry* btreeState,
                                 DiskLoc bucket, int keyOffset) const {
            const BtreeBucket<Version> *b = getBucket(btreeState,bucket);
            return b->keyNode(keyOffset).recordLoc;
        }

        virtual void keyAndRecordAt(const IndexCatalogEntry* btreeState,
                                    DiskLoc bucket, int keyOffset, BSONObj* keyOut,
                                    DiskLoc* recordOut) const {
            verify(!bucket.isNull());
            const BtreeBucket<Version> *b = getBucket(btreeState,bucket);

            int n = b->getN();

            // If n is 0xffff the bucket was deleted.
            if (keyOffset < 0 || keyOffset >= n || n == 0xffff || !b->isUsed(keyOffset)) {
                return;
            }

            if (keyOffset >= n) {
                *keyOut = BSONObj();
                *recordOut = DiskLoc();
            } else {
                *keyOut = b->keyNode(keyOffset).key.toBson();
                *recordOut = b->keyNode(keyOffset).recordLoc;
            }
        }

        virtual string dupKeyError(const IndexCatalogEntry* btreeState,
                                   DiskLoc bucket,
                                   const BSONObj& keyObj) const {
            typename Version::KeyOwned key(keyObj);
            return getBucket( btreeState, bucket )->dupKeyError(btreeState->descriptor(),
                                                                key);
        }

        virtual DiskLoc advance(const IndexCatalogEntry* btreeState,
                                const DiskLoc& thisLoc,
                                int& keyOfs,
                                int direction,
                                const char* caller) const {
            return getBucket( btreeState, thisLoc )->advance(thisLoc, keyOfs, direction, caller);
        }

        virtual long long fullValidate(const IndexCatalogEntry* btreeState,
                                       const DiskLoc& thisLoc,
                                       const BSONObj& keyPattern) {
            return getBucket( btreeState, thisLoc )->fullValidate(thisLoc, keyPattern);
        }
    };

    BtreeInterfaceImpl<V0> interface_v0;
    BtreeInterfaceImpl<V1> interface_v1;
    BtreeInterface* BtreeInterface::interfaces[] = { &interface_v0, &interface_v1 };

}  // namespace mongo

