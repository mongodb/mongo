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

#include "mongo/db/btree.h"
#include "mongo/db/index/btree_interface.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    template <class Version>
    class BtreeInterfaceImpl : public BtreeInterface {
    public:
        // typedef typename BucketBasics<Version>::VersionNode VersionNode;

        virtual ~BtreeInterfaceImpl() { }

        virtual int bt_insert(const DiskLoc thisLoc,
                              const DiskLoc recordLoc,
                              const BSONObj& key,
                              const Ordering &order,
                              bool dupsAllowed,
                              IndexDetails& idx,
                              bool toplevel) const {
            // FYI: toplevel has a default value of true in btree.h
            return thisLoc.btree<Version>()->bt_insert(
                thisLoc,
                recordLoc,
                key,
                order,
                dupsAllowed,
                idx,
                toplevel);
        }

        virtual bool unindex(const DiskLoc thisLoc,
                             IndexDetails& id,
                             const BSONObj& key,
                             const DiskLoc recordLoc) const {
            return thisLoc.btree<Version>()->unindex(thisLoc, id, key, recordLoc);
        }

        virtual DiskLoc locate(const IndexDetails& idx,
                               const DiskLoc& thisLoc,
                               const BSONObj& key,
                               const Ordering& order,
                               int& pos,
                               bool& found,
                               const DiskLoc& recordLoc,
                               int direction) const {
            // FYI: direction has a default of 1
            return thisLoc.btree<Version>()->locate(
                idx,
                thisLoc,
                key,
                order,
                pos,
                found,
                recordLoc,
                direction);
        }

        virtual bool wouldCreateDup(const IndexDetails& idx,
                                    const DiskLoc& thisLoc,
                                    const BSONObj& key,
                                    const Ordering& order,
                                    const DiskLoc& self) const {
            typename Version::KeyOwned ownedVersion(key);
            return thisLoc.btree<Version>()->wouldCreateDup(
                idx,
                thisLoc,
                ownedVersion,
                order,
                self);
        }

        virtual void customLocate(DiskLoc& locInOut,
                                  int& keyOfs,
                                  const BSONObj& keyBegin,
                                  int keyBeginLen, bool afterVersion,
                                  const vector<const BSONElement*>& keyEnd,
                                  const vector<bool>& keyEndInclusive,
                                  const Ordering& order,
                                  int direction,
                                  pair<DiskLoc, int>& bestParent) {
            locInOut.btree<Version>()->customLocate(
                locInOut,
                keyOfs,
                keyBegin,
                keyBeginLen,
                afterVersion,
                keyEnd,
                keyEndInclusive,
                order,
                direction,
                bestParent);
        }

        virtual void advanceTo(DiskLoc &thisLoc,
                               int &keyOfs,
                               const BSONObj &keyBegin,
                               int keyBeginLen,
                               bool afterVersion,
                               const vector<const BSONElement*>& keyEnd,
                               const vector<bool>& keyEndInclusive,
                               const Ordering& order, int direction) const {
            thisLoc.btree<Version>()->advanceTo(
                thisLoc,
                keyOfs,
                keyBegin,
                keyBeginLen,
                afterVersion,
                keyEnd,
                keyEndInclusive,
                order,
                direction);
        }


        virtual bool keyIsUsed(DiskLoc bucket, int keyOffset) const {
            return bucket.btree<Version>()->k(keyOffset).isUsed();
        }

        virtual BSONObj keyAt(DiskLoc bucket, int keyOffset) const {
            verify(!bucket.isNull());
            const BtreeBucket<Version> *b = bucket.btree<Version>();
            int n = b->getN();
            if (n == b->INVALID_N_SENTINEL) {
                throw UserException(deletedBucketCode, "keyAt bucket deleted");
            }
            dassert( n >= 0 && n < 10000 );
            return keyOffset >= n ? BSONObj() : b->keyNode(keyOffset).key.toBson();
        }

        virtual DiskLoc recordAt(DiskLoc bucket, int keyOffset) const {
            const BtreeBucket<Version> *b = bucket.btree<Version>();
            return b->keyNode(keyOffset).recordLoc;
        }

        virtual void keyAndRecordAt(DiskLoc bucket, int keyOffset, BSONObj* keyOut,
                                    DiskLoc* recordOut) const {
            verify(!bucket.isNull());
            const BtreeBucket<Version> *b = bucket.btree<Version>();

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

        virtual string dupKeyError(DiskLoc bucket, const IndexDetails &idx,
                                   const BSONObj& keyObj) const {
            typename Version::KeyOwned key(keyObj);
            return bucket.btree<Version>()->dupKeyError(idx, key);
        }

        virtual DiskLoc advance(const DiskLoc& thisLoc,
                                int& keyOfs,
                                int direction,
                                const char* caller) const {
            return thisLoc.btree<Version>()->advance(thisLoc, keyOfs, direction, caller);
        }

        virtual long long fullValidate(const DiskLoc& thisLoc, const BSONObj& keyPattern) {
            return thisLoc.btree<Version>()->fullValidate(thisLoc, keyPattern);
        }
    };

    BtreeInterfaceImpl<V0> interface_v0;
    BtreeInterfaceImpl<V1> interface_v1;
    BtreeInterface* BtreeInterface::interfaces[] = { &interface_v0, &interface_v1 };

}  // namespace mongo
