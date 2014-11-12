// kv_record_store.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include <algorithm>
#include <boost/static_assert.hpp>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/dictionary/kv_dictionary_update.h"
#include "mongo/db/storage/kv/dictionary/kv_record_store.h"
#include "mongo/db/storage/kv/slice.h"

#include "mongo/platform/endian.h"
#include "mongo/util/log.h"

namespace mongo {

    /**
     * Class to abstract the in-memory vs on-disk format of a key in the
     * record dictionary.
     *
     * In memory, a key is a valid DiskLoc for which loc.isValid() and
     * !loc.isNull() are true.
     *
     * On disk, a key is a pair of back-to-back integers whose individual
     * 4 bytes values are serialized in big-endian format. This allows the
     * entire structure to be compared with memcmp while still maintaining
     * the property that the filenum field compares more significantly
     * than the offset field. Being able to compare with memcmp is
     * important because for most storage engines, memcmp is the default
     * and fastest comparator.
     */
    class RecordIdKey {
        DiskLoc _loc;
        uint64_t _key;

        // We assume that a diskloc can be represented in a 64 bit integer.
        BOOST_STATIC_ASSERT(sizeof(DiskLoc) == sizeof(uint64_t));

    public:
        /**
         * Used when we have a diskloc and we want a memcmp-ready Slice to
         * be used as a stored key in the KVDictionary.
         *
         * Algorithm:
         * - Take a diskloc with two integers laid out in native bye order
         *   [a, o]
         * - Construct a 64 bit integer whose high order bits are `a' and
         *   whose low order bits are `o'
         * - Convert that integer to big endian and store it in `_key'
         */
        RecordIdKey(const DiskLoc &loc) :
            _loc(loc),
            _key() {
            _key = endian::nativeToBig(uint64_t(loc.a()) << 32ULL |
                                       uint64_t(loc.getOfs()));
        }

        /**
         * Used when we have a big-endian key Slice from the KVDictionary
         * and we want to get its diskloc representation.
         *
         * Algorithm (work backwards from the above constructor's
         * algorithm):
         * - Interpret the stored key as a 64 bit integer into `_key',
         *   then convert `_key' to native byte order and store it in `k'.
         * - Create a diskloc(a, o) where `a' is a 32 bit integer
         *   constructed from the high order bits of `_k' and where `o' is
         *   constructed from the low order bits.
         */
        RecordIdKey(const Slice &key) :
            _loc(),
            _key(key.as<uint64_t>()) {
            uint64_t k = endian::bigToNative(_key);
            _loc = DiskLoc((k & 0xFFFFFFFF00000000) >> 32ULL,
                            k & 0x00000000FFFFFFFF);
        }

        /**
         * Return an un-owned slice of _key, suitable for use as a key
         * into the KVDictionary that maps record ids to record data.
         */
        Slice key() const {
            return Slice::of(_key);
        }

        /**
         * Return the DiskLoc representation of a deserialized key Slice
         */
        DiskLoc loc() const {
            return _loc;
        }
    };

    KVRecordStore::KVRecordStore( KVDictionary *db,
                                  OperationContext* opCtx,
                                  const StringData& ns,
                                  const StringData& ident,
                                  const CollectionOptions& options )
        : RecordStore(ns),
          _db(db),
          _metadataDict(NULL),
          _numRecordsMetadataKey(numRecordsMetadataKey(ident)),
          _dataSizeMetadataKey(dataSizeMetadataKey(ident))
    {
        invariant(_db != NULL);

        // Get the next id, which is one greater than the greatest stored.
        boost::scoped_ptr<RecordIterator> iter(getIterator(opCtx, DiskLoc(), CollectionScanParams::BACKWARD));
        if (!iter->isEOF()) {
            const DiskLoc lastLoc = iter->curr();
            _nextIdNum.store(lastLoc.getOfs() + (uint64_t(lastLoc.a()) << 32ULL) + 1);
        } else {
            // Need to start at 1 so we are always higher than minDiskLoc
            _nextIdNum.store(1);
        }
    }

    int64_t KVRecordStore::_getStats(OperationContext *opCtx, const std::string &key) const {
        Slice valSlice;
        Status s = _metadataDict->get(opCtx, Slice(key), valSlice);
        invariant(s.isOK());
        return mongo::endian::littleToNative(valSlice.as<int64_t>());
    }

    void KVRecordStore::_updateStats(OperationContext *opCtx, int64_t numRecordsDelta, int64_t dataSizeDelta) {
        if (_metadataDict) {
            KVUpdateIncrementMessage nrMessage(numRecordsDelta);
            Status s = _metadataDict->update(opCtx, Slice(_numRecordsMetadataKey), nrMessage);
            invariant(s.isOK());

            KVUpdateIncrementMessage dsMessage(dataSizeDelta);
            s = _metadataDict->update(opCtx, Slice(_dataSizeMetadataKey), dsMessage);
            invariant(s.isOK());
        }
    }

    void KVRecordStore::_initializeStatsForKey(OperationContext *opCtx, const std::string &key) {
        Slice val;
        Status s = _metadataDict->get(opCtx, Slice(key), val);
        if (s.code() == ErrorCodes::NoSuchKey) {
            int64_t zero = 0;
            s = _metadataDict->insert(opCtx, Slice(key), Slice::of(zero));
        }
        invariant(s.isOK());
    }

    void KVRecordStore::setStatsMetadataDictionary(OperationContext *opCtx, KVDictionary *metadataDict) {
        _metadataDict = metadataDict;
        _initializeStatsForKey(opCtx, _numRecordsMetadataKey);
        _initializeStatsForKey(opCtx, _dataSizeMetadataKey);
    }

    void KVRecordStore::deleteMetadataKeys(OperationContext *opCtx, KVDictionary *metadataDict, const StringData &ident) {
        Status s = metadataDict->remove(opCtx, Slice(numRecordsMetadataKey(ident)));
        invariant(s.isOK());
        s = metadataDict->remove(opCtx, Slice(dataSizeMetadataKey(ident)));
        invariant(s.isOK());
    }

    long long KVRecordStore::dataSize( OperationContext* txn ) const {
        if (_metadataDict) {
            return _getStats(txn, _dataSizeMetadataKey);
        } else {
            return _db->getStats().dataSize;
        }
    }

    long long KVRecordStore::numRecords( OperationContext* txn ) const {
        if (_metadataDict) {
            return _getStats(txn, _numRecordsMetadataKey);
        } else {
            return _db->getStats().numKeys;
        }
    }

    int64_t KVRecordStore::storageSize( OperationContext* txn,
                                        BSONObjBuilder* extraInfo,
                                        int infoLevel ) const {
        return _db->getStats().storageSize;
    }

    RecordData KVRecordStore::_getDataFor(const KVDictionary *db, OperationContext* txn, const DiskLoc& loc) {
        const RecordIdKey key(loc);

        Slice value;
        Status status = db->get(txn, key.key(), value);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::NoSuchKey) {
                return RecordData(nullptr, 0);
            } else {
                log() << "storage engine get() failed, operation will fail: " << status.toString();
                uasserted(28549, status.toString());
            }
        }

        // Return an owned RecordData that uses the shared array from `value'
        return RecordData(value.data(), value.size(), value.ownedBuf());
    }

    RecordData KVRecordStore::dataFor( OperationContext* txn, const DiskLoc& loc) const {
        RecordData rd;
        bool found = findRecord(txn, loc, &rd);
        // This method is called when we know there must be an associated record for `loc'
        invariant(found);
        return rd;
    }

    bool KVRecordStore::findRecord( OperationContext* txn,
                                    const DiskLoc& loc, RecordData* out ) const {
        RecordData rd = _getDataFor(_db.get(), txn, loc);
        if (rd.data() == NULL) {
            return false;
        }
        *out = rd;
        return true;
    }

    void KVRecordStore::deleteRecord( OperationContext* txn, const DiskLoc& loc ) {
        const RecordIdKey key(loc);

        Slice val;
        Status status = _db->get(txn, key.key(), val);
        invariant(status.isOK());

        _updateStats(txn, -1, -val.size());

        status = _db->remove( txn, key.key() );
        invariant(status.isOK());
    }

    StatusWith<DiskLoc> KVRecordStore::insertRecord( OperationContext* txn,
                                                     const char* data,
                                                     int len,
                                                     bool enforceQuota ) {
        const DiskLoc loc = _nextId();
        const RecordIdKey key(loc);
        const Slice value(data, len);

        DEV {
            // Should never overwrite an existing record.
            Slice v;
            const Status status = _db->get(txn, key.key(), v);
            invariant(status.code() == ErrorCodes::NoSuchKey);
        }

        const Status status = _db->insert(txn, key.key(), value);
        if (!status.isOK()) {
            return StatusWith<DiskLoc>(status);
        }

        _updateStats(txn, +1, value.size());

        return StatusWith<DiskLoc>(loc);
    }

    StatusWith<DiskLoc> KVRecordStore::insertRecord( OperationContext* txn,
                                                     const DocWriter* doc,
                                                     bool enforceQuota ) {
        Slice value(doc->documentSize());
        doc->writeDocument(value.mutableData());
        return insertRecord(txn, value.data(), value.size(), enforceQuota);
    }

    StatusWith<DiskLoc> KVRecordStore::updateRecord( OperationContext* txn,
                                                     const DiskLoc& loc,
                                                     const char* data,
                                                     int len,
                                                     bool enforceQuota,
                                                     UpdateMoveNotifier* notifier ) {
        const RecordIdKey key(loc);
        const Slice value(data, len);

        int64_t numRecordsDelta = 0;
        int64_t dataSizeDelta = value.size();

        Slice val;
        Status status = _db->get(txn, key.key(), val);
        if (status.code() == ErrorCodes::NoSuchKey) {
            numRecordsDelta += 1;
        } else if (status.isOK()) {
            dataSizeDelta -= val.size();
        } else {
            return StatusWith<DiskLoc>(status);
        }

        // An update with a complete new image (data, len) is implemented as an overwrite insert.
        status = _db->insert(txn, key.key(), value);
        if (!status.isOK()) {
            return StatusWith<DiskLoc>(status);
        }

        _updateStats(txn, numRecordsDelta, dataSizeDelta);

        return StatusWith<DiskLoc>(loc);
    }

    Status KVRecordStore::updateWithDamages( OperationContext* txn,
                                             const DiskLoc& loc,
                                             const RecordData& oldRec,
                                             const char* damageSource,
                                             const mutablebson::DamageVector& damages ) {
        const RecordIdKey key(loc);

        const Slice oldValue(oldRec.data(), oldRec.size());
        const KVUpdateWithDamagesMessage message(damageSource, damages);

        // updateWithDamages can't change the number or size of records, so we don't need to update
        // stats.

        return _db->update(txn, key.key(), oldValue, message);
    }

    RecordIterator* KVRecordStore::getIterator( OperationContext* txn,
                                                const DiskLoc& start,
                                                const CollectionScanParams::Direction& dir
                                              ) const {
        return new KVRecordIterator(_db.get(), txn, start, dir);
    }

    RecordIterator* KVRecordStore::getIteratorForRepair( OperationContext* txn ) const {
        return getIterator(txn);
    }

    std::vector<RecordIterator *> KVRecordStore::getManyIterators( OperationContext* txn ) const {
        std::vector<RecordIterator *> iterators;
        iterators.push_back(getIterator(txn));
        return iterators;
    }

    Status KVRecordStore::truncate( OperationContext* txn ) {
        // This is not a very performant implementation of truncate.
        //
        // At the time of this writing, it is only used by 'emptycapped', a test-only command.
        for (boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
             !iter->isEOF(); ) {
            DiskLoc loc = iter->getNext();
            deleteRecord( txn, loc );
        }

        return Status::OK();
    }

    Status KVRecordStore::compact( OperationContext* txn,
                                   RecordStoreCompactAdaptor* adaptor,
                                   const CompactOptions* options,
                                   CompactStats* stats ) {
        return _db->compact( txn );
    }

    Status KVRecordStore::validate( OperationContext* txn,
                                    bool full,
                                    bool scanData,
                                    ValidateAdaptor* adaptor,
                                    ValidateResults* results,
                                    BSONObjBuilder* output ) const {
        bool invalidObject = false;
        size_t numRecords = 0;
        for (boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
             !iter->isEOF(); ) {
            numRecords++;
            if (scanData) {
                RecordData data = dataFor( txn, iter->curr() );
                size_t dataSize;
                if (full) {
                    const Status status = adaptor->validate( data, &dataSize );
                    if (!status.isOK()) {
                        results->valid = false;
                        if ( invalidObject ) {
                            results->errors.push_back("invalid object detected (see logs)");
                        }
                        invalidObject = true;
                        log() << "Invalid object detected in " << _ns << ": " << status.reason();
                    }
                }
            }
            iter->getNext();
        }
        output->appendNumber("nrecords", numRecords);

        return Status::OK();
    }

    void KVRecordStore::appendCustomStats( OperationContext* txn,
                                           BSONObjBuilder* result,
                                           double scale ) const {
        _db->appendCustomStats(txn, result, scale);
    }

    Status KVRecordStore::touch( OperationContext* txn, BSONObjBuilder* output ) const {
        Timer t;
        for (boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
             !iter->isEOF(); iter->getNext()) {
            // no-op, data is brought into memory just by iterating over it
        }

        if (output) {
            output->append("numRanges", 1);
            output->append("millis", t.millis());
        }
        return Status::OK();
    }

    Status KVRecordStore::setCustomOption( OperationContext* txn,
                                           const BSONElement& option,
                                           BSONObjBuilder* info ) {
        return _db->setCustomOption( txn, option, info );
    }

    DiskLoc KVRecordStore::_nextId() {
        const uint64_t myId = _nextIdNum.fetchAndAdd(1);
        int a = myId >> 32;
        // This masks the lowest 4 bytes of myId
        int ofs = myId & 0x00000000FFFFFFFF;
        DiskLoc loc( a, ofs );
        return loc;
    }

    // ---------------------------------------------------------------------- //

    void KVRecordStore::KVRecordIterator::_setCursor(const DiskLoc loc) {
        // We should no cursor at this point, either because we're getting newly
        // constructed or because we're recovering from saved state (and so
        // the old cursor needed to be dropped).
        invariant(!_cursor);
        _cursor.reset();
        _savedLoc = DiskLoc();
        _savedVal = Slice();

        invariant(loc.isValid() && !loc.isNull());
        const RecordIdKey key(loc);
        _cursor.reset(_db->getCursor(_txn, _dir));
        _cursor->seek(key.key());
    }

    KVRecordStore::KVRecordIterator::KVRecordIterator(KVDictionary *db, OperationContext *txn,
                                       const DiskLoc &start,
                                       const CollectionScanParams::Direction &dir) :
        _db(db), _dir(dir), _savedLoc(DiskLoc()), _savedVal(Slice()), _txn(txn), _cursor() {
        if (start.isNull()) {
            // A null diskloc means the beginning for a forward cursor,
            // and the end for a reverse cursor.
            _setCursor(_dir == CollectionScanParams::FORWARD ? minDiskLoc : maxDiskLoc);
        } else {
            _setCursor(start);
        }
    }

    bool KVRecordStore::KVRecordIterator::isEOF() {
        return !_cursor || !_cursor->ok();
    }

    DiskLoc KVRecordStore::KVRecordIterator::curr() {
        if (isEOF()) {
            return DiskLoc();
        }

        const RecordIdKey key(_cursor->currKey());
        return key.loc();
    }

    void KVRecordStore::KVRecordIterator::_saveLocAndVal() {
        if (!isEOF()) {
            _savedLoc = curr();
            _savedVal = _cursor->currVal().owned();
        } else {
            _savedLoc = DiskLoc();
            _savedVal = Slice();
        }
    }

    DiskLoc KVRecordStore::KVRecordIterator::getNext() {
        if (isEOF()) {
            return DiskLoc();
        }

        // We need valid copies of _savedLoc / _savedVal since we are
        // about to advance the underlying cursor.
        _saveLocAndVal();
        _cursor->advance();
        return _savedLoc;
    }

    void KVRecordStore::KVRecordIterator::invalidate(const DiskLoc& loc) {
        // this only gets called to invalidate potentially buffered
        // `loc' results between saveState() and restoreState(). since
        // we dropped our cursor and have no buffered rows, we do nothing.
    }

    void KVRecordStore::KVRecordIterator::saveState() {
        // we need to drop the current cursor because it was created with
        // an operation context that the caller intends to close after
        // this function finishes (and before restoreState() is called,
        // which will give us a new operation context)
        _saveLocAndVal();
        _cursor.reset();
        _txn = NULL;
    }

    bool KVRecordStore::KVRecordIterator::restoreState(OperationContext* txn) {
        invariant(!_txn && !_cursor);
        _txn = txn;
        if (!_savedLoc.isNull()) {
            _setCursor(_savedLoc);
        } else {
            // We had saved state when the cursor was at EOF, so the savedLoc
            // was null - therefore we must restoreState to EOF as well. 
            //
            // Assert that this is indeed the case.
            invariant(isEOF());
        }

        // `true' means the collection still exists, which is always the case
        // because this cursor would have been deleted by higher layers if
        // the collection were to indeed be dropped.
        return true;
    }

    RecordData KVRecordStore::KVRecordIterator::dataFor(const DiskLoc& loc) const {
        invariant(_txn);

        // Kind-of tricky:
        //
        // We save the last loc and val that we were pointing to before a call
        // to getNext(). We know that our caller intends to call dataFor() on
        // each loc read this way, so if the given loc is equal to the last 
        // loc, then we can return the last value read, which we own and now
        // pass to the caller with a shared pointer.
        if (!_savedLoc.isNull() && _savedLoc == loc) {
            Slice val = _savedVal;
            invariant(val.ownedBuf());
            return RecordData(val.data(), val.size(), val.ownedBuf());
        } else {
            // .. otherwise something strange happened and the caller actually
            // wants some other data entirely. we should probably never execute
            // this code that often because it is slow to descend the dictionary
            // for every value we want to read..
            return _getDataFor(_db, _txn, loc);
        }
    }

} // namespace mongo
