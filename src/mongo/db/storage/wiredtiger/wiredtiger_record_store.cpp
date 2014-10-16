// wiredtiger_record_store.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 WiredTiger Inc.
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

#include <wiredtiger.h>

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

//#define RS_ITERATOR_TRACE(x) log() << "WTRS::Iterator " << x
#define RS_ITERATOR_TRACE(x)

namespace mongo {

    std::string WiredTigerRecordStore::generateCreateString( const CollectionOptions& options,
                                                             const StringData& extraStrings ) {
        // Separate out a prefix and suffix in the default string. User configuration will
        // override values in the prefix, but not values in the suffix.
        std::stringstream ss;
        if ( 0 ) {
            ss << "type=file,";
            ss << "leaf_page_max=512k,";
            ss << "memory_page_max=10m,";
        }
        else {
            ss << "type=lsm";
        }

        ss << extraStrings << ",";

        ss << "key_format=q,value_format=u";
        return ss.str();
    }

    WiredTigerRecordStore::WiredTigerRecordStore( OperationContext* ctx,
                                                  const StringData& ns,
                                                  const StringData& uri,
                                                  bool isCapped,
                                                  int64_t cappedMaxSize,
                                                  int64_t cappedMaxDocs,
                                                  CappedDocumentDeleteCallback* cappedDeleteCallback )
        : RecordStore( ns ),
          _uri( uri.toString() ),
          _instanceId( WiredTigerSession::genCursorId() ),
          _isCapped( isCapped ),
          _cappedMaxSize( cappedMaxSize ),
          _cappedMaxDocs( cappedMaxDocs ),
          _cappedDeleteCallback( cappedDeleteCallback ) {

        if (_isCapped) {
            invariant(_cappedMaxSize > 0);
            invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
        }
        else {
            invariant(_cappedMaxSize == -1);
            invariant(_cappedMaxDocs == -1);
        }

        /*
         * Find the largest DiskLoc currently in use and estimate the number of records.  We don't
         * have an operation context, so we can't use an Iterator.
         */
        scoped_ptr<RecordIterator> iterator( getIterator( ctx, DiskLoc(), false,
                                                          CollectionScanParams::BACKWARD ) );
        if ( iterator->isEOF() ) {
            _dataSize = 0;
            _numRecords = 0;
            // Need to start at 1 so we are always higher than minDiskLoc
            _nextIdNum.store( 1 );
        }
        else {
            uint64_t max = _makeKey( iterator->curr() );
            _nextIdNum.store( 1 + max );

            // todo: this is bad
            _numRecords = 0;
            _dataSize = 0;

            while( !iterator->isEOF() ) {
                DiskLoc loc = iterator->getNext();
                RecordData data = iterator->dataFor( loc );
                _numRecords++;
                _dataSize += data.size();
            }

        }

    }

    WiredTigerRecordStore::~WiredTigerRecordStore() { }

    long long WiredTigerRecordStore::dataSize( OperationContext *txn ) const {
        return _dataSize;
    }

    long long WiredTigerRecordStore::numRecords( OperationContext *txn ) const {
        return _numRecords;
    }

    bool WiredTigerRecordStore::isCapped() const {
        return _isCapped;
    }

    void WiredTigerRecordStore::setCapped(int64_t cappedMaxSize, int64_t cappedMaxDocs) {
        _isCapped = true;
        _cappedMaxSize = cappedMaxSize;
        _cappedMaxDocs = cappedMaxDocs;
    }

    int64_t WiredTigerRecordStore::cappedMaxDocs() const {
        invariant(_isCapped);
        return _cappedMaxDocs;
    }

    int64_t WiredTigerRecordStore::cappedMaxSize() const {
        invariant(_isCapped);
        return _cappedMaxSize;
    }

    int64_t WiredTigerRecordStore::storageSize( OperationContext* txn,
                                           BSONObjBuilder* extraInfo,
                                           int infoLevel ) const {
        int64_t size = dataSize(txn);
        if ( size == 0 && _isCapped ) {
            // Many things assume anempty capped collection still takes up space.
            return 1;
        }
        return size;
    }

    // Retrieve the value from a positioned cursor.
    RecordData WiredTigerRecordStore::_getData(const WiredTigerCursor& cursor) const {
        WT_ITEM value;
        int ret = cursor->get_value(cursor.get(), &value);
        invariantWTOK(ret);

        boost::shared_array<char> data( new char[value.size] );
        memcpy( data.get(), value.data, value.size );
        return RecordData(reinterpret_cast<const char *>(data.get()), value.size, data );
    }

    RecordData WiredTigerRecordStore::dataFor(OperationContext* txn, const DiskLoc& loc) const {
        // ownership passes to the shared_array created below
        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        c->set_key(c, _makeKey(loc));
        int ret = c->search(c);
        massert( 28534,
                 "Didn't find DiskLoc in WiredTigerRecordStore",
                 ret != WT_NOTFOUND );
        invariantWTOK(ret);
        return _getData(curwrap);
    }

    bool WiredTigerRecordStore::findRecord( OperationContext* txn,
                                            const DiskLoc& loc, RecordData* out ) const {
        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        c->set_key(c, _makeKey(loc));
        int ret = c->search(c);
        if ( ret == WT_NOTFOUND )
            return false;
        invariantWTOK(ret);
        *out = _getData(curwrap);
        return true;
    }

    void WiredTigerRecordStore::deleteRecord( OperationContext* txn, const DiskLoc& loc ) {
        WiredTigerCursor cursor( _uri, _instanceId, txn );
        WT_CURSOR *c = cursor.get();
        c->set_key(c, _makeKey(loc));
        int ret = c->search(c);
        invariantWTOK(ret);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariantWTOK(ret);

        int old_length = old_value.size;

        ret = c->remove(c);
        invariantWTOK(ret);

        _changeNumRecords(txn, false);
        _increaseDataSize(txn, -old_length);
    }

    bool WiredTigerRecordStore::cappedAndNeedDelete(OperationContext* txn) const {
        if (!_isCapped)
            return false;

        if (_dataSize > _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (numRecords(txn) > _cappedMaxDocs))
            return true;

        return false;
    }

    void WiredTigerRecordStore::cappedDeleteAsNeeded(OperationContext* txn) {
        if (!cappedAndNeedDelete(txn))
            return;

        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        int ret = c->next(c);
        while ( ret == 0 && cappedAndNeedDelete(txn) ) {
            invariant(_numRecords > 0);

            uint64_t key;
            ret = c->get_key(c, &key);
            invariantWTOK(ret);
            DiskLoc oldest = _fromKey(key);

            if ( _cappedDeleteCallback )
                uassertStatusOK(_cappedDeleteCallback->aboutToDeleteCapped(txn, oldest));

            deleteRecord(txn, oldest);
            ret = c->next(c);
        }

        if (ret != WT_NOTFOUND) invariantWTOK(ret);
    }

    StatusWith<DiskLoc> WiredTigerRecordStore::insertRecord( OperationContext* txn,
                                                             const char* data,
                                                             int len,
                                                             bool enforceQuota ) {
        if ( _isCapped && len > _cappedMaxSize ) {
            return StatusWith<DiskLoc>( ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize" );
        }

        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        DiskLoc loc = _nextId();
        c->set_key(c, _makeKey(loc));
        WiredTigerItem value(data, len);
        c->set_value(c, value.Get());
        int ret = c->insert(c);
        invariantWTOK(ret);

        _changeNumRecords( txn, true );
        _increaseDataSize( txn, len );

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>( loc );
    }

    StatusWith<DiskLoc> WiredTigerRecordStore::insertRecord( OperationContext* txn,
                                                        const DocWriter* doc,
                                                        bool enforceQuota ) {
        const int len = doc->documentSize();

        if ( _isCapped && len > _cappedMaxSize ) {
            return StatusWith<DiskLoc>( ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize" );
        }

        boost::shared_array<char> buf( new char[len] );
        doc->writeDocument( buf.get() );

        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        DiskLoc loc = _nextId();
        c->set_key(c, _makeKey(loc));
        WiredTigerItem value(buf.get(), len);
        c->set_value(c, value.Get());
        int ret = c->insert(c);
        invariantWTOK(ret);

        _changeNumRecords( txn, true );
        _increaseDataSize( txn, len );

        cappedDeleteAsNeeded( txn );

        return StatusWith<DiskLoc>( loc );
    }

    StatusWith<DiskLoc> WiredTigerRecordStore::updateRecord( OperationContext* txn,
                                                        const DiskLoc& loc,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota,
                                                        UpdateMoveNotifier* notifier ) {
        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        c->set_key(c, _makeKey(loc));
        int ret = c->search(c);
        invariantWTOK(ret);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariantWTOK(ret);

        int old_length = old_value.size;

        c->set_key(c, _makeKey(loc));
        WiredTigerItem value(data, len);
        c->set_value(c, value.Get());
        ret = c->update(c);
        invariantWTOK(ret);

        _increaseDataSize(txn, len - old_length);

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>( loc );
    }

    Status WiredTigerRecordStore::updateWithDamages( OperationContext* txn,
                                                const DiskLoc& loc,
                                                const char* damangeSource,
                                                const mutablebson::DamageVector& damages ) {
        // get original value
        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        c->set_key(c, _makeKey(loc));
        int ret = c->search(c);
        invariantWTOK(ret);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariantWTOK(ret);

        std::string data(reinterpret_cast<const char *>(old_value.data), old_value.size);

        // apply changes to our copy
        char* root = const_cast<char*>( data.c_str() );
        for( size_t i = 0; i < damages.size(); i++ ) {
            mutablebson::DamageEvent event = damages[i];
            const char* sourcePtr = damangeSource + event.sourceOffset;
            char* targetPtr = root + event.targetOffset;
            std::memcpy(targetPtr, sourcePtr, event.size);
        }

        // write back
        WiredTigerItem value(data);
        c->set_value(c, value.Get());
        ret = c->update(c);
        invariantWTOK(ret);

        return Status::OK();
    }

    RecordIterator* WiredTigerRecordStore::getIterator( OperationContext* txn,
                                                   const DiskLoc& start,
                                                   bool tailable,
                                                   const CollectionScanParams::Direction& dir
                                                   ) const {
        return new Iterator(*this, txn, start, tailable, dir);
    }


    RecordIterator* WiredTigerRecordStore::getIteratorForRepair( OperationContext* txn ) const {
        return getIterator( txn );
    }

    std::vector<RecordIterator*> WiredTigerRecordStore::getManyIterators(
            OperationContext* txn ) const {
        // XXX do we want this to actually return a set of iterators?

        std::vector<RecordIterator*> iterators;
        iterators.push_back( getIterator( txn ) );
        return iterators;
    }

    Status WiredTigerRecordStore::truncate( OperationContext* txn ) {
        // TODO: use a WiredTiger fast truncate
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        while( !iter->isEOF() ) {
            DiskLoc loc = iter->getNext();
            deleteRecord( txn, loc );
        }

        // WiredTigerRecoveryUnit* ru = _getRecoveryUnit( txn );

        return Status::OK();
    }

    Status WiredTigerRecordStore::compact( OperationContext* txn,
                                      RecordStoreCompactAdaptor* adaptor,
                                      const CompactOptions* options,
                                      CompactStats* stats ) {
        WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(txn)->getSessionCache();
        WiredTigerSession* session = cache->getSession();
        WT_SESSION *s = session->getSession();
        int ret = s->compact(s, GetURI().c_str(), NULL);
        invariantWTOK(ret);
        cache->releaseSession(session);
        return Status::OK();
    }

    Status WiredTigerRecordStore::validate( OperationContext* txn,
                                       bool full, bool scanData,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results,
                                       BSONObjBuilder* output ) const {

        long long nrecords = 0;
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        results->valid = true;
        while( !iter->isEOF() ) {
            ++nrecords;
            if ( full && scanData ) {
                size_t dataSize;
                DiskLoc loc = iter->curr();
                RecordData data = dataFor( txn, loc );
                Status status = adaptor->validate( data, &dataSize );
                if ( !status.isOK() ) {
                    results->valid = false;
                    results->errors.push_back( loc.toString() + " is corrupted" );
                }
            }
            iter->getNext();
        }

        output->appendNumber( "nrecords", nrecords );
        return Status::OK();
    }

    void WiredTigerRecordStore::appendCustomStats( OperationContext* txn,
                                              BSONObjBuilder* result,
                                              double scale ) const {
        result->appendBool( "capped", _isCapped );
        if ( _isCapped ) {
            result->appendIntOrLL( "max", _cappedMaxDocs );
            result->appendIntOrLL( "maxSize", _cappedMaxSize );
        }
        WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession();
        WT_SESSION* s = session->getSession();
        invariant(s);
        WT_CURSOR* c = NULL;
        int ret;
        BSONObjBuilder bob(result->subobjStart("wiredtiger"));
        const string uri = "statistics:" + GetURI();
        bob.append("uri", uri);
        if ((ret = s->open_cursor(s, uri.c_str(), NULL, "statistics=(fast)", &c)) != 0) {
            bob.append("error", "unable to retrieve statistics");
            bob.append("message", wiredtiger_strerror(ret));
        }
        else {
            invariant(c);
            ON_BLOCK_EXIT(c->close, c);
            const char *desc, *pvalue;
            uint64_t value;
            while (c->next(c) == 0 &&
                   c->get_value(c, &desc, &pvalue, &value) == 0) {
                bob.append(desc, pvalue);
            }
        }
    }


    Status WiredTigerRecordStore::touch( OperationContext* txn, BSONObjBuilder* output ) const {
        if (output) {
            output->append("numRanges", 1);
            output->append("millis", 0);
        }
        return Status::OK();
    }

    Status WiredTigerRecordStore::setCustomOption( OperationContext* txn,
                                              const BSONElement& option,
                                              BSONObjBuilder* info ) {
        string optionName = option.fieldName();
        if ( !option.isBoolean() ) {
            return Status( ErrorCodes::BadValue, "Invalid Value" );
        }
        // TODO: expose some WiredTiger configurations
        if ( optionName == "usePowerOf2Sizes" ) {
            return Status::OK();
        } else
        if ( optionName.compare( "verify_checksums" ) == 0 ) {
        }
        else
            return Status( ErrorCodes::InvalidOptions, "Invalid Option" );

        return Status::OK();
    }

    DiskLoc WiredTigerRecordStore::_nextId() {
        const uint64_t myId = _nextIdNum.fetchAndAdd(1);
        int a = myId >> 32;
        // This masks the lowest 4 bytes of myId
        int ofs = myId & 0x00000000FFFFFFFF;
        DiskLoc loc( a, ofs );
        return loc;
    }

    WiredTigerRecoveryUnit* WiredTigerRecordStore::_getRecoveryUnit( OperationContext* txn ) {
        return dynamic_cast<WiredTigerRecoveryUnit*>( txn->recoveryUnit() );
    }

    void WiredTigerRecordStore::_changeNumRecords( OperationContext* txn, bool insert ) {
        if ( insert ) {
            _numRecords++;
        }
        else {
            _numRecords--;
        }
    }


    void WiredTigerRecordStore::_increaseDataSize( OperationContext* txn, int amount ) {
        _dataSize += amount;
        // TODO make this persistent
    }

    uint64_t WiredTigerRecordStore::_makeKey( const DiskLoc& loc ) {
        return ((uint64_t)loc.a() << 32 | loc.getOfs());
    }
    DiskLoc WiredTigerRecordStore::_fromKey( uint64_t key ) {
        uint32_t a = key >> 32;
        uint32_t ofs = (uint32_t)key;
        return DiskLoc(a, ofs);
    }

    // --------

    WiredTigerRecordStore::Iterator::Iterator(
            const WiredTigerRecordStore& rs,
            OperationContext *txn,
            const DiskLoc& start,
            bool tailable,
            const CollectionScanParams::Direction& dir )
        : _rs( rs ),
          _txn( txn ),
          _tailable( tailable ),
          _dir( dir ),
          _cursor( new WiredTigerCursor( rs.GetURI(), rs.instanceId(), txn ) ) {
        RS_ITERATOR_TRACE("start");
        _locate(start, true);
    }

    WiredTigerRecordStore::Iterator::~Iterator() {
    }

    void WiredTigerRecordStore::Iterator::_locate(const DiskLoc &loc, bool exact) {
        RS_ITERATOR_TRACE("_locate " << loc);
        WT_CURSOR *c = _cursor->get();
        invariant( c );
        int ret;
        if (loc.isNull()) {
            ret = _forward() ? c->next(c) : c->prev(c);
            if (ret != WT_NOTFOUND) invariantWTOK(ret);
            _eof = (ret == WT_NOTFOUND);
            RS_ITERATOR_TRACE("_locate   null loc eof: " << _eof);
            return;
        }
        c->set_key(c, _makeKey(loc));
        if (exact) {
            ret = c->search(c);
        }
        else {
            // If loc doesn't exist, inexact matches should find the first existing record before
            // it, in the direction of the scan. Note that inexact callers will call _getNext()
            // after locate so they actually return the record *after* the one we seek to.
            int cmp;
            ret = c->search_near(c, &cmp);
            invariantWTOK(ret);
            if (_forward()) {
                // return <= loc
                if (cmp > 0)
                    ret = c->prev(c);
            }
            else {
                // return >= loc
                if (cmp < 0)
                    ret = c->next(c);
            }
        }
        if (ret != WT_NOTFOUND) invariantWTOK(ret);
        _eof = (ret == WT_NOTFOUND);
        RS_ITERATOR_TRACE("_locate   not null loc eof: " << _eof);
    }

    bool WiredTigerRecordStore::Iterator::isEOF() {
        RS_ITERATOR_TRACE( "isEOF start " << _eof << " " << _lastLoc );
        if (_eof && _tailable && !_lastLoc.isNull()) {
            WiredTigerRecoveryUnit* ru = WiredTigerRecoveryUnit::get( _txn );
            invariant( !ru->everStartedWrite() );
            ru->restartTransaction();

            DiskLoc saved = _lastLoc;
            _locate(_lastLoc, true);
            RS_ITERATOR_TRACE( "isEOF check " << _eof );
            if ( _eof ) {
                _lastLoc = DiskLoc();
            }
            else if ( _curr() != saved ) {
                RS_ITERATOR_TRACE( "isEOF wrap " << _curr() );
                // wrapped around :(
                _lastLoc = DiskLoc();
                _eof = true;
            }
            else {
                // we found where we left off!
                // now we advance to the next one
                RS_ITERATOR_TRACE( "isEOF found " << _curr() );
                _getNext();
            }
        }
        RS_ITERATOR_TRACE( "isEOF end " << _eof );
        return _eof;
    }

    // Allow const functions to use curr to find current location.
    DiskLoc WiredTigerRecordStore::Iterator::_curr() const {
        RS_ITERATOR_TRACE( "_curr" );
        if (_eof)
            return DiskLoc();

        WT_CURSOR *c = _cursor->get();
        invariant( c );
        uint64_t key;
        int ret = c->get_key(c, &key);
        invariantWTOK(ret);
        return _fromKey(key);
    }

    DiskLoc WiredTigerRecordStore::Iterator::curr() {
        return _curr();
    }

    void WiredTigerRecordStore::Iterator::_getNext() {
        RS_ITERATOR_TRACE("_getNext");
        WT_CURSOR *c = _cursor->get();
        int ret = _forward() ? c->next(c) : c->prev(c);
        if (ret != WT_NOTFOUND) invariantWTOK(ret);
        _eof = (ret == WT_NOTFOUND);
        RS_ITERATOR_TRACE("_getNext " << ret << " " << _eof );
        if ( !_eof ) {
            RS_ITERATOR_TRACE("_getNext " << ret << " " << _eof << " " << _curr() );
        }
    }

    DiskLoc WiredTigerRecordStore::Iterator::getNext() {
        RS_ITERATOR_TRACE( "getNext" );
        /* Take care not to restart a scan if we have hit the end */
        if (isEOF())
            return DiskLoc();

        /* MongoDB expects "natural" ordering - which is the order that items are inserted. */
        DiskLoc toReturn = curr();
        RS_ITERATOR_TRACE( "getNext toReturn: " << toReturn );
        _getNext();
        RS_ITERATOR_TRACE( " ----" );
        _lastLoc = toReturn;
        return toReturn;
    }

    void WiredTigerRecordStore::Iterator::invalidate( const DiskLoc& dl ) {
        // this should never be called
    }

    void WiredTigerRecordStore::Iterator::saveState() {
        RS_ITERATOR_TRACE("saveState");

        // the cursor and recoveryUnit are valid on restore
        // so we just record the recoveryUnit to make sure
        _savedRecoveryUnit = _txn->recoveryUnit();
        _txn = NULL;
    }

    bool WiredTigerRecordStore::Iterator::restoreState( OperationContext *txn ) {

        // This is normally already the case, but sometimes we are given a new
        // OperationContext on restore - update the iterators context in that
        // case
        _txn = txn;
        invariant( _savedRecoveryUnit == txn->recoveryUnit() );
        return true;
    }

    RecordData WiredTigerRecordStore::Iterator::dataFor( const DiskLoc& loc ) const {
        // Retrieve the data if the iterator is already positioned at loc, otherwise
        // open a new cursor and find the data to avoid upsetting the iterators
        // cursor position.
        if (loc == _curr())
            return (_rs._getData(*_cursor));
        else
            return (_rs.dataFor( _txn, loc ));
    }

    bool WiredTigerRecordStore::Iterator::_forward() const {
        return _dir == CollectionScanParams::FORWARD;
    }

    void WiredTigerRecordStore::temp_cappedTruncateAfter( OperationContext* txn,
                                                     DiskLoc end,
                                                     bool inclusive ) {
        WriteUnitOfWork wuow(txn);
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        if ( iter->isEOF() )
            return;
        while( !iter->isEOF() ) {
            DiskLoc loc = iter->getNext();
            if ( end < loc || ( inclusive && end == loc ) )
                deleteRecord( txn, loc );
        }
        wuow.commit();
    }
}
