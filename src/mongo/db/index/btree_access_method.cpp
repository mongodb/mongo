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

#include "mongo/db/index/btree_access_method.h"

#include <vector>

#include "mongo/db/index/btree_index_cursor.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/pdfile_private.h"

namespace mongo {

    template <class Key> BtreeBasedAccessMethod<Key>::BtreeBasedAccessMethod(
        IndexDescriptor *descriptor) : _descriptor(descriptor),
                                       _ordering(Ordering::make(_descriptor->keyPattern())) { }

    // Find the keys for obj, put them in the tree pointing to loc
    template <class Key> Status BtreeBasedAccessMethod<Key>::insert(const BSONObj& obj,
        const DiskLoc& loc, const InsertDeleteOptions& options, int64_t* numInserted) {

        *numInserted = 0;

        BSONObjSet keys;
        // Delegate to the subclass.
        getKeys(obj, &keys);

        Status ret = Status::OK();

        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            try {
                _descriptor->getHead().btree<Key>()->bt_insert(
                    _descriptor->getHead(), loc, *i, _ordering, options.dupsAllowed,
                    _descriptor->getOnDisk(), true);
                ++*numInserted;
            } catch (AssertionException& e) {
                if (10287 == e.getCode() && options.dupsAllowed) {
                    // Duplicate key, but our options say to ignore it.
                    DEV log() << "info: key already in index during bg indexing (ok)\n";
                } else if (!options.dupsAllowed) {
                    // Assuming it's a duplicate key exception.  Clean up any inserted keys.
                    for (BSONObjSet::const_iterator j = keys.begin(); j != i; ++j) {
                        removeOneKey(*j, loc);
                    }
                    *numInserted = 0;
                    return Status(ErrorCodes::DuplicateKey, e.what(), e.getCode());
                } else {
                    problem() << " caught assertion addKeysToIndex "
                              << _descriptor->indexNamespace()
                              << obj["_id"] << endl;
                    ret = Status(ErrorCodes::InternalError, e.what(), e.getCode());
                }
            }
        }

        return ret;
    }

    template <class Key> bool BtreeBasedAccessMethod<Key>::removeOneKey(const BSONObj& key,
        const DiskLoc& loc) {

        bool ret = false;

        try {
            ret = _descriptor->getHead().btree<Key>()->unindex(_descriptor->getHead(),
                _descriptor->getOnDisk(), key, loc);
        } catch (AssertionException& e) {
            problem() << "Assertion failure: _unindex failed "
                << _descriptor->indexNamespace() << endl;
            out() << "Assertion failure: _unindex failed: " << e.what() << '\n';
            out() << "  obj:" << loc.obj().toString() << '\n';
            out() << "  key:" << key.toString() << '\n';
            out() << "  dl:" << loc.toString() << endl;
            logContext();
        }

        return ret;
    }

    // Remove the provided doc from the index.
    template <class Key> Status BtreeBasedAccessMethod<Key>::remove(
        const BSONObj &obj, const DiskLoc& loc, const InsertDeleteOptions &options,
        int64_t* numDeleted) {

        BSONObjSet keys;
        getKeys(obj, &keys);
        *numDeleted = 0;

        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            bool thisKeyOK = removeOneKey(*i, loc);

            if (thisKeyOK) {
                ++*numDeleted;
            } else if (options.logIfError) {
                log() << "unindex failed (key too big?) " << _descriptor->indexNamespace()
                      << " key: " << *i << " " << loc.obj()["_id"] << endl;
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

    template <class Key> Status BtreeBasedAccessMethod<Key>::touch(const BSONObj &obj) {
        BSONObjSet keys;
        getKeys(obj, &keys);

        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            DiskLoc unusedDiskLoc;
            int unusedPos;
            bool unusedFound;
            _descriptor->getHead().btree<Key>()->locate(_descriptor->getOnDisk(),
                _descriptor->getHead(), *i, _ordering, unusedPos, unusedFound, unusedDiskLoc, 1);
        }

        return Status::OK();
    }

    template <class Key> Status BtreeBasedAccessMethod<Key>::validateUpdate(
        const BSONObj &from, const BSONObj &to, const DiskLoc &record,
        const InsertDeleteOptions &options, UpdateTicket* status) {

        BtreeBasedPrivateUpdateData *data = new BtreeBasedPrivateUpdateData();
        status->_indexSpecificUpdateData.reset(data);

        getKeys(from, &data->oldKeys);
        getKeys(to, &data->newKeys);
        data->loc = record;
        data->dupsAllowed = options.dupsAllowed;

        status->_isMultiKey = data->newKeys.size() > 1;

        setDifference(data->oldKeys, data->newKeys, &data->removed);
        setDifference(data->newKeys, data->oldKeys, &data->added);

        // Check for dups.
        if (!data->added.empty() && _descriptor->unique() && !options.dupsAllowed) {
            const BtreeBucket<Key> *head = _descriptor->getHead().btree<Key>();
            for (vector<BSONObj*>::iterator i = data->added.begin(); i != data->added.end(); i++) {
                typename Key::KeyOwned key(**i);
                if (head->wouldCreateDup(_descriptor->getOnDisk(), _descriptor->getHead(),
                                         key, _ordering, record)) {
                    status->_isValid = false;
                    return Status(ErrorCodes::DuplicateKey,
                        head->dupKeyError(_descriptor->getOnDisk(), key));
                }
            }
        }

        status->_isValid = true;

        return Status::OK();
    }

    template <class Key> Status BtreeBasedAccessMethod<Key>::update(const UpdateTicket& ticket) {
        if (!ticket._isValid) {
            return Status(ErrorCodes::InternalError, "Invalid updateticket in update");
        }

        BtreeBasedPrivateUpdateData *data =
            static_cast<BtreeBasedPrivateUpdateData*>(ticket._indexSpecificUpdateData.get());

        for (size_t i = 0; i < data->added.size(); ++i) {
            _descriptor->getHead().btree<Key>()->bt_insert(_descriptor->getHead(), data->loc,
                *data->added[i], _ordering, data->dupsAllowed, _descriptor->getOnDisk(), true);
        }

        for (size_t i = 0; i < data->removed.size(); ++i) {
            _descriptor->getHead().btree<Key>()->unindex(_descriptor->getHead(),
                _descriptor->getOnDisk(), *data->removed[i], data->loc);
        }

        return Status::OK();
    }

    // Standard Btree implementation below.
    template <class Key> BtreeAccessMethod<Key>::BtreeAccessMethod(IndexDescriptor *descriptor)
        : BtreeBasedAccessMethod<Key>(descriptor) {

        // The key generation wants these values.
        vector<const char*> fieldNames;
        vector<BSONElement> fixed;

        BSONObjIterator it(_descriptor->keyPattern());
        while (it.more()) {
            BSONElement elt = it.next();
            fieldNames.push_back(elt.fieldName());
            fixed.push_back(BSONElement());
        }

        if (0 == descriptor->version()) {
            _keyGenerator.reset(new BtreeKeyGeneratorV0(fieldNames, fixed,
                _descriptor->isSparse()));
        } else if (1 == descriptor->version()) {
            _keyGenerator.reset(new BtreeKeyGeneratorV1(fieldNames, fixed,
                _descriptor->isSparse()));
        } else {
            massert(16745, "Invalid index version for key generation.", false );
        }
    }

    template <class Key> void BtreeAccessMethod<Key>::getKeys(const BSONObj &obj,
                                                              BSONObjSet *keys) {
        _keyGenerator->getKeys(obj, keys);
    }

    template <class Key> Status BtreeAccessMethod<Key>::newCursor(IndexCursor **out) {
        *out = new BtreeIndexCursor<Key>(_descriptor, _ordering);
        return Status::OK();
    }

    template class BtreeBasedAccessMethod<V0>;
    template class BtreeBasedAccessMethod<V1>;

    template class BtreeAccessMethod<V0>;
    template class BtreeAccessMethod<V1>;

}  // namespace mongo
