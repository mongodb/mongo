// wiredtiger_record_store.cpp

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

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include <wiredtiger.h>

namespace mongo {

    int WiredTigerRecordStore::Create(WiredTigerDatabase &db,
            const StringData &ns, const CollectionOptions &options, bool allocateDefaultSpace) {
        WiredTigerSession swrap(db.GetSession(), db);
        WT_SESSION *s(swrap.Get());
        return s->create(s, _getURI(ns).c_str(), "type=file,key_format=u,value_format=u");
    }

    WiredTigerRecordStore::WiredTigerRecordStore( const StringData& ns,
                                        WiredTigerDatabase &db,
                                        bool isCapped,
                                        int64_t cappedMaxSize,
                                        int64_t cappedMaxDocs,
                                        CappedDocumentDeleteCallback* cappedDeleteCallback )
        : RecordStore( ns ),
          _db( db ),
          _uri (_getURI(ns) ),
          _isCapped( isCapped ),
          _cappedMaxSize( cappedMaxSize ),
          _cappedMaxDocs( cappedMaxDocs ),
          _cappedDeleteCallback( cappedDeleteCallback ),
          _numRecords( 0 ) {
        if (_isCapped) {
            invariant(_cappedMaxSize > 0);
            invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
        }
        else {
            invariant(_cappedMaxSize == -1);
            invariant(_cappedMaxDocs == -1);
        }
    }

    WiredTigerRecordStore::~WiredTigerRecordStore() { }

    long long WiredTigerRecordStore::dataSize() const {
        return _dataSize;
    }

    long long WiredTigerRecordStore::numRecords() const {
        return _numRecords;
    }

    bool WiredTigerRecordStore::isCapped() const {
        return _isCapped;
    }

    int64_t WiredTigerRecordStore::storageSize( OperationContext* txn,
                                           BSONObjBuilder* extraInfo,
                                           int infoLevel ) const {
        return dataSize(); // todo: this isn't very good
    }

    RecordData WiredTigerRecordStore::dataFor( const DiskLoc& loc) const {
        // ownership passes to the shared_array created below
        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        c->set_key(c, _makeKey(loc).Get());
        int ret = c->search(c);
        invariant(ret == 0);

        WT_ITEM value;
        ret = c->get_value(c, &value);
        invariant(ret == 0);

        boost::shared_array<char> data( new char[value.size] );
        memcpy( data.get(), value.data, value.size );
        return RecordData(reinterpret_cast<const char *>(data.get()), value.size, data );
    }

    void WiredTigerRecordStore::deleteRecord( OperationContext* txn, const DiskLoc& loc ) {
        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        c->set_key(c, _makeKey(loc).Get());
        int ret = c->search(c);
        invariant(ret == 0);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariant(ret == 0);

        int old_length = old_value.size;

        ret = c->remove(c);
        invariant(ret == 0);

        _changeNumRecords(txn, false);
        _increaseDataSize(txn, -old_length);
    }

    bool WiredTigerRecordStore::cappedAndNeedDelete() const {
        if (!_isCapped)
            return false;

        if (_dataSize > _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (numRecords() > _cappedMaxDocs))
            return true;

        return false;
    }

    void WiredTigerRecordStore::cappedDeleteAsNeeded(OperationContext* txn) {
        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        int ret = c->next(c);
        while ( ret == 0 && cappedAndNeedDelete() ) {
            invariant(_numRecords > 0);

            WT_ITEM key;
            ret = c->get_key(c, &key);
            invariant(ret == 0);
            DiskLoc oldest = reinterpret_cast<const DiskLoc*>(key.data)[0];

            if ( _cappedDeleteCallback )
                uassertStatusOK(_cappedDeleteCallback->aboutToDeleteCapped(txn, oldest));

            ret = c->remove(c);
            invariant(ret == 0);
            ret = c->next(c);
        }

        invariant(ret == 0 || ret == WT_NOTFOUND);
    }

    StatusWith<DiskLoc> WiredTigerRecordStore::insertRecord( OperationContext* txn,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota ) {
        if ( _isCapped && len > _cappedMaxSize ) {
            return StatusWith<DiskLoc>( ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize" );
        }

        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        DiskLoc loc = _nextId();
        c->set_key(c, _makeKey(loc).Get());
        c->set_value(c, WiredTigerItem(data, len).Get());
        int ret = c->insert(c);
        invariant(ret == 0);

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

        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        DiskLoc loc = _nextId();
        c->set_key(c, _makeKey(loc).Get());
        c->set_value(c, WiredTigerItem(buf.get(), len).Get());
        int ret = c->insert(c);
        invariant(ret == 0);

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
        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        c->set_key(c, _makeKey(loc).Get());
        int ret = c->search(c);
        invariant(ret == 0);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariant(ret == 0);

        int old_length = old_value.size;

        c->set_value(c, WiredTigerItem(data, len).Get());
        ret = c->insert(c);
        invariant(ret == 0);

        _increaseDataSize(txn, len - old_length);

        cappedDeleteAsNeeded(txn);

        return StatusWith<DiskLoc>( loc );
    }

    Status WiredTigerRecordStore::updateWithDamages( OperationContext* txn,
                                                const DiskLoc& loc,
                                                const char* damangeSource,
                                                const mutablebson::DamageVector& damages ) {
        // get original value
        WiredTigerSession swrap(_db);
        WiredTigerCursor curwrap(GetCursor(swrap), swrap);
        WT_CURSOR *c = curwrap.Get();
        c->set_key(c, _makeKey(loc).Get());
        int ret = c->search(c);
        invariant(ret == 0);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariant(ret == 0);

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
        c->set_value(c, WiredTigerItem(data).Get());
        ret = c->insert(c);
        invariant(ret == 0);

        return Status::OK();
    }

    RecordIterator* WiredTigerRecordStore::getIterator( OperationContext* txn,
                                                   const DiskLoc& start,
                                                   bool tailable,
                                                   const CollectionScanParams::Direction& dir
                                                   ) const {
        invariant( start == DiskLoc() );
        invariant( !tailable );
        //
        // XXX leak -- we need a session associated with the txn.
        WiredTigerSession *session = new WiredTigerSession(_db.GetSession(), _db);
        return new Iterator(*this, *session, dir);
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
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        while( !iter->isEOF() ) {
            DiskLoc loc = iter->getNext();
            deleteRecord( txn, loc );
        }

        // TODO: clear current transaction
        // WiredTigerRecoveryUnit* ru = _getRecoveryUnit( txn );

        return Status::OK();
    }

    Status WiredTigerRecordStore::compact( OperationContext* txn,
                                      RecordStoreCompactAdaptor* adaptor,
                                      const CompactOptions* options,
                                      CompactStats* stats ) {
        WiredTigerSession swrap(_db.GetSession(), _db);
        WT_SESSION *s(swrap.Get());
        int ret = s->compact(s, GetURI().c_str(), NULL);
        invariant(ret == 0);
        return Status::OK();
    }

    Status WiredTigerRecordStore::validate( OperationContext* txn,
                                       bool full, bool scanData,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results,
                                       BSONObjBuilder* output ) const {
        RecordIterator* iter = getIterator( txn );
        while( !iter->isEOF() ) {
            RecordData data = dataFor( iter->curr() );
            if ( scanData ) {
                BSONObj b( data.data() );
                if ( !b.valid() ) {
                    DiskLoc l = iter->curr();
                    results->errors.push_back( l.toString() + " is corrupted" );
                }
            }
            iter->getNext();
        }
        return Status::OK();
    }

    void WiredTigerRecordStore::appendCustomStats( OperationContext* txn,
                                              BSONObjBuilder* result,
                                              double scale ) const {
    }


    // AFB: is there a way to force column families to be cached in rocks?
    Status WiredTigerRecordStore::touch( OperationContext* txn, BSONObjBuilder* output ) const {
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
        if ( optionName.compare( "verify_checksums" ) == 0 ) {
        }
        else
            return Status( ErrorCodes::BadValue, "Invalid Option" );

        return Status::OK();
    }

    DiskLoc WiredTigerRecordStore::_nextId() {
        boost::mutex::scoped_lock lk( _idLock );
        int ofs = _nextIdNum >> 32;
        int a = (_nextIdNum << 32 ) >> 32;
        DiskLoc loc( a, ofs );
        _nextIdNum++;
        return loc;
    }

    WiredTigerRecoveryUnit* WiredTigerRecordStore::_getRecoveryUnit( OperationContext* opCtx ) {
        return dynamic_cast<WiredTigerRecoveryUnit*>( opCtx->recoveryUnit() );
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

    WiredTigerItem WiredTigerRecordStore::_makeKey( const DiskLoc& loc ) {
        return WiredTigerItem( reinterpret_cast<const char*>( &loc ), sizeof( loc ) );
    }

    // --------

    WiredTigerRecordStore::Iterator::Iterator(const WiredTigerRecordStore& rs,
            WiredTigerSession &session, const CollectionScanParams::Direction& dir )
        : _rs( rs ),
          _session( session ),
          _dir( dir ),
          // XXX not using a snapshot here
          _cursor(rs.GetCursor(session), session),
          _eof( true ) {
        (void)getNext();
    }

    bool WiredTigerRecordStore::Iterator::isEOF() {
        return _eof;
    }

    DiskLoc WiredTigerRecordStore::Iterator::curr() {
        if (_eof)
            return DiskLoc();

        WT_CURSOR *c = _cursor.Get();
        WT_ITEM key;
        int ret = c->get_key(c, &key);
        invariant(ret == 0);
        return reinterpret_cast<const DiskLoc*>( key.data )[0];
    }

    DiskLoc WiredTigerRecordStore::Iterator::getNext() {
        DiskLoc toReturn = curr();
        WT_CURSOR *c = _cursor.Get();
        /*
         * MongoDB expects "natural" ordering - which is the order that items are inserted.
         * So a forward iteration in WiredTiger starts at the end of a collection.
         */
        int ret = _forward() ? c->prev(c) : c->next(c);
        invariant(ret == 0 || ret == WT_NOTFOUND);
        _eof = (ret == WT_NOTFOUND);
        return toReturn;
    }

    void WiredTigerRecordStore::Iterator::invalidate( const DiskLoc& dl ) {
        WT_CURSOR *c = _cursor.Get();
        int ret = c->reset(c);
        invariant(ret == 0);
        _eof = true;
    }

    void WiredTigerRecordStore::Iterator::saveState() {
        // TODO delete iterator, store information
    }

    bool WiredTigerRecordStore::Iterator::restoreState() {
        // TODO set iterator to same place as before, but with new snapshot
        return true;
    }

    RecordData WiredTigerRecordStore::Iterator::dataFor( const DiskLoc& loc ) const {
        WT_CURSOR *c = _cursor.Get();
        WT_ITEM key;
        int ret = c->get_key(c, &key);
        invariant(ret == 0);
        DiskLoc curr = reinterpret_cast<const DiskLoc*>( key.data )[0];
        if (curr == loc) {
            WT_ITEM value;
            int ret = c->get_value(c, &value);
            invariant(ret == 0);

            boost::shared_array<char> data( new char[value.size] );
            memcpy( data.get(), value.data, value.size );
            return RecordData(reinterpret_cast<const char *>(data.get()), value.size, data );
        }

        return _rs.dataFor( loc );
    }

    bool WiredTigerRecordStore::Iterator::_forward() const {
        return _dir == CollectionScanParams::FORWARD;
    }

    void WiredTigerRecordStore::temp_cappedTruncateAfter( OperationContext* txn,
                                                     DiskLoc end,
                                                     bool inclusive ) {
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        if ( iter->isEOF() )
            return;
        while( !iter->isEOF() ) {
            DiskLoc loc = iter->getNext();
            if ( end < loc || ( inclusive && end == loc ) )
                deleteRecord( txn, loc );
        }
    }
}
