// kv_sorted_data_impl.cpp

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


#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/kv/dictionary/kv_dictionary.h"
#include "mongo/db/storage/kv/dictionary/kv_sorted_data_impl.h"
#include "mongo/db/storage/kv/slice.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    const int kTempKeyMaxSize = 1024; // Do the same as the heap implementation

    namespace {

        /**
         * Strips the field names from a BSON object
         */
        BSONObj stripFieldNames( const BSONObj& obj ) {
            BSONObjBuilder b;
            BSONObjIterator i( obj );
            while ( i.more() ) {
                BSONElement e = i.next();
                b.appendAs( e, "" );
            }
            return b.obj();
        }

        /**
         * Constructs a string containing the bytes of key followed by the bytes of loc.
         *
         * @param removeFieldNames true if the field names in key should be replaced with empty
         * strings, and false otherwise. Useful because field names are not necessary in an index
         * key, because the ordering of the fields is already known.
         */
        Slice makeString( const BSONObj& key, const DiskLoc loc, bool removeFieldNames = true ) {
            const BSONObj finalKey = removeFieldNames ? stripFieldNames( key ) : key;

            Slice s(finalKey.objsize() + sizeof loc);

            std::copy(finalKey.objdata(), finalKey.objdata() + finalKey.objsize(), s.mutableData());
            DiskLoc *lp = reinterpret_cast<DiskLoc *>(s.mutableData() + finalKey.objsize());
            *lp = loc;

            return s;
        }

        /**
         * Constructs an IndexKeyEntry from a slice containing the bytes of a BSONObject followed
         * by the bytes of a DiskLoc
         */
        IndexKeyEntry makeIndexKeyEntry( const Slice& slice ) {
            BSONObj key = BSONObj( slice.data() );
            DiskLoc loc = *reinterpret_cast<const DiskLoc*>( slice.data() + key.objsize() );
            return IndexKeyEntry( key, loc );
        }

        /**
         * Creates an error code message out of a key
         */
        string dupKeyError(const BSONObj& key) {
            stringstream ss;
            ss << "E11000 duplicate key error ";
            // TODO figure out how to include index name without dangerous casts
            ss << "dup key: " << key.toString();
            return ss.str();
        }

    }  // namespace

    KVSortedDataImpl::KVSortedDataImpl( KVDictionary* db,
                                        OperationContext* opCtx,
                                        const IndexDescriptor* desc) :
        _db( db ) {
        invariant( _db );
    }

    Status KVSortedDataBuilderImpl::addKey(const BSONObj& key, const DiskLoc& loc) {
        return _impl->insert(_txn, key, loc, _dupsAllowed);
    }

    SortedDataBuilderInterface* KVSortedDataImpl::getBulkBuilder(OperationContext* txn,
                                                                 bool dupsAllowed) {
      return new KVSortedDataBuilderImpl(this, txn, dupsAllowed);
    }

    Status KVSortedDataImpl::insert(OperationContext* txn,
                                    const BSONObj& key,
                                    const DiskLoc& loc,
                                    bool dupsAllowed) {
        if (key.objsize() >= kTempKeyMaxSize) {
            const string msg = mongoutils::str::stream()
                               << "KVSortedDataImpl::insert() key too large to index, failing "
                               << key.objsize() << ' ' << key;
            return Status(ErrorCodes::KeyTooLong, msg);
        }

        if (!dupsAllowed) {
            Status status = dupKeyCheck(txn, key, loc);
            if (!status.isOK()) {
                return status;
            }
        }

        _db->insert(txn, makeString(key, loc), Slice());

        return Status::OK();
    }

    void KVSortedDataImpl::unindex(OperationContext* txn,
                                   const BSONObj& key,
                                   const DiskLoc& loc,
                                   bool dupsAllowed) {
        _db->remove(txn, makeString(key, loc));
    }

    Status KVSortedDataImpl::dupKeyCheck(OperationContext* txn,
                                         const BSONObj& key,
                                         const DiskLoc& loc) {
        boost::scoped_ptr<SortedDataInterface::Cursor> cursor(newCursor(txn, 1));
        cursor->locate(key, DiskLoc());

        if (cursor->isEOF() || cursor->getKey() != key) {
            return Status::OK();
        } else if (cursor->getDiskLoc() == loc) {
            return Status::OK();
        } else {
            return Status(ErrorCodes::DuplicateKey, dupKeyError(key));
        }
    }

    void KVSortedDataImpl::fullValidate(OperationContext* txn, long long* numKeysOut) const {
        if (numKeysOut) {
            *numKeysOut = 0;
            for (boost::scoped_ptr<KVDictionary::Cursor> cursor(_db->getCursor(txn));
                 cursor->ok(); cursor->advance()) {
                ++(*numKeysOut);
            }
        }
    }

    bool KVSortedDataImpl::isEmpty( OperationContext* txn ) {
        boost::scoped_ptr<KVDictionary::Cursor> cursor(_db->getCursor(txn));
        return !cursor->ok();
    }

    Status KVSortedDataImpl::touch(OperationContext* txn) const {
        // fullValidate iterates over every key, which brings things into memory
        long long numKeys;
        fullValidate(txn, &numKeys);
        return Status::OK();
    }

    long long KVSortedDataImpl::numEntries(OperationContext* txn) const {
        long long numKeys = 0;
        fullValidate(txn, &numKeys);
        return numKeys;
    }

    Status KVSortedDataImpl::initAsEmpty(OperationContext* txn) {
        // no-op
        return Status::OK();
    }

    long long KVSortedDataImpl::getSpaceUsedBytes( OperationContext* txn ) const {
        KVDictionary::Stats stats = _db->getStats();
        return stats.storageSize;
    }

    // ---------------------------------------------------------------------- //

    class KVSortedDataInterfaceCursor : public SortedDataInterface::Cursor {
        KVDictionary *_db;
        const int _dir;
        OperationContext *_txn;

        boost::scoped_ptr<KVDictionary::Cursor> _cursor;
        BSONObj _savedKey;
        DiskLoc _savedLoc;

        bool _locate(const BSONObj &key, const DiskLoc &loc) {
            _cursor.reset(_db->getCursor(_txn, _dir));
            _cursor->seek(makeString(key, loc, false));
            return !isEOF() && loc == getDiskLoc() && key == getKey();
        }

    public:
        KVSortedDataInterfaceCursor(KVDictionary *db, OperationContext *txn, int direction)
            : _db(db),
              _dir(direction),
              _txn(txn),
              _cursor(db->getCursor(txn, _dir)),
              _savedKey(),
              _savedLoc() {
            invariant(_cursor);
        }

        virtual ~KVSortedDataInterfaceCursor() {}

        int getDirection() const {
            return _dir;
        }

        bool isEOF() const {
            return !_cursor || !_cursor->ok();
        }

        bool pointsToSamePlaceAs(const Cursor& other) const {
            return getDiskLoc() == other.getDiskLoc() && getKey() == other.getKey();
        }

        void aboutToDeleteBucket(const DiskLoc& bucket) { }

        bool locate(const BSONObj& key, const DiskLoc& loc) {
            return _locate(stripFieldNames(key), loc);
        }

        void advanceTo(const BSONObj &keyBegin,
                       int keyBeginLen,
                       bool afterKey,
                       const vector<const BSONElement*>& keyEnd,
                       const vector<bool>& keyEndInclusive) {
            // make a key representing the location to which we want to advance.
            BSONObj key = IndexEntryComparison::makeQueryObject(
                                                                keyBegin,
                                                                keyBeginLen,
                                                                afterKey,
                                                                keyEnd,
                                                                keyEndInclusive,
                                                                getDirection() );
            _locate(key, _dir > 0 ? minDiskLoc : maxDiskLoc);
        }

        void customLocate(const BSONObj& keyBegin,
                          int keyBeginLen,
                          bool afterVersion,
                          const vector<const BSONElement*>& keyEnd,
                          const vector<bool>& keyEndInclusive) {
            // The rocks engine has this to say:
            // XXX I think these do the same thing????
            advanceTo( keyBegin, keyBeginLen, afterVersion, keyEnd, keyEndInclusive );
        }

        BSONObj getKey() const {
            if (isEOF()) {
                return BSONObj();
            }
            IndexKeyEntry entry = makeIndexKeyEntry(_cursor->currKey());
            return entry.key;
        }

        DiskLoc getDiskLoc() const {
            if (isEOF()) {
                return DiskLoc();
            }
            IndexKeyEntry entry = makeIndexKeyEntry(_cursor->currKey());
            return entry.loc;
        }

        void advance() {
            if (!isEOF()) {
                _cursor->advance();
            }
        }

        void savePosition() {
            _savedKey = getKey().getOwned();
            _savedLoc = getDiskLoc();
            _cursor.reset();
            _txn = NULL;
        }

        void restorePosition(OperationContext* txn) {
            invariant(!_txn && !_cursor);
            _txn = txn;
            if (!_savedKey.isEmpty() && !_savedLoc.isNull()) {
                _locate(_savedKey, _savedLoc);
            } else {
                invariant(_savedKey.isEmpty() && _savedLoc.isNull());
                invariant(isEOF()); // this is the whole point!
            }
        }
    };

    SortedDataInterface::Cursor* KVSortedDataImpl::newCursor(OperationContext* txn,
                                                             int direction) const {
        return new KVSortedDataInterfaceCursor(_db.get(), txn, direction);
    }

} // namespace mongo
