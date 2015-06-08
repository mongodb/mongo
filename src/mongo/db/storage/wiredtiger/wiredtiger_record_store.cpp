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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

#include <boost/shared_array.hpp>
#include <wiredtiger.h>

#include "mongo/base/checked_cast.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

//#define RS_ITERATOR_TRACE(x) log() << "WTRS::Iterator " << x
#define RS_ITERATOR_TRACE(x)

namespace mongo {

    using std::unique_ptr;
    using std::string;

namespace {

    static const int kMinimumRecordStoreVersion = 1;
    static const int kCurrentRecordStoreVersion = 1; // New record stores use this by default.
    static const int kMaximumRecordStoreVersion = 1;
    BOOST_STATIC_ASSERT(kCurrentRecordStoreVersion >= kMinimumRecordStoreVersion);
    BOOST_STATIC_ASSERT(kCurrentRecordStoreVersion <= kMaximumRecordStoreVersion);

    bool shouldUseOplogHack(OperationContext* opCtx, const std::string& uri) {
        StatusWith<BSONObj> appMetadata = WiredTigerUtil::getApplicationMetadata(opCtx, uri);
        if (!appMetadata.isOK()) {
            return false;
        }

        return (appMetadata.getValue().getIntField("oplogKeyExtractionVersion") == 1);
    }

} // namespace

    MONGO_FP_DECLARE(WTWriteConflictException);

    const std::string kWiredTigerEngineName = "wiredTiger";

    const long long WiredTigerRecordStore::kCollectionScanOnCreationThreshold = 10000;

    class WiredTigerRecordStore::Cursor final : public RecordCursor {
    public:
        Cursor(OperationContext* txn,
               const WiredTigerRecordStore& rs,
               bool forward = true,
               bool forParallelCollectionScan = false)
            : _rs(rs)
            , _txn(txn)
            , _forward(forward)
            , _forParallelCollectionScan(forParallelCollectionScan)
            , _cursor(new WiredTigerCursor(rs.getURI(), rs.instanceId(), true, txn))
            , _readUntilForOplog(WiredTigerRecoveryUnit::get(txn)->getOplogReadTill())
        {}

        boost::optional<Record> next() final {
            if (_eof) return {};

            WT_CURSOR* c = _cursor->get();
            {
                // Nothing after the next line can throw WCEs.
                // Note that an unpositioned (or eof) WT_CURSOR returns the first/last entry in the
                // table when you call next/prev.
                int advanceRet = WT_OP_CHECK(_forward ? c->next(c) : c->prev(c));
                if (advanceRet == WT_NOTFOUND) {
                    _eof = true;
                    return {};
                }
                invariantWTOK(advanceRet);
            }

            int64_t key;
            invariantWTOK(c->get_key(c, &key));
            const RecordId id = _fromKey(key);

            if (!isVisible(id)) {
                _eof = true;
                return {};
            }

            WT_ITEM value;
            invariantWTOK(c->get_value(c, &value));
            auto data = RecordData(static_cast<const char*>(value.data), value.size);
            data.makeOwned(); // TODO delete this line once safe.

            _lastReturnedId = id;
            return {{id, std::move(data)}};
        }

        boost::optional<Record> seekExact(const RecordId& id) final {
            if (!isVisible(id)) {
                _eof = true;
                return {};
            }

            WT_CURSOR* c = _cursor->get();
            c->set_key(c, _makeKey(id));
            // Nothing after the next line can throw WCEs.
            int seekRet = WT_OP_CHECK(c->search(c));
            if (seekRet == WT_NOTFOUND) {
                _eof = true;
                return {};
            }
            invariantWTOK(seekRet);

            WT_ITEM value;
            invariantWTOK(c->get_value(c, &value));
            auto data = RecordData(static_cast<const char*>(value.data), value.size);
            data.makeOwned(); // TODO delete this line once safe.

            _lastReturnedId = id;
            return {{id, std::move(data)}};
        }

        void savePositioned() final {
            // It must be safe to call save() twice in a row without calling restore().
            if (!_txn) return;

            // the cursor and recoveryUnit are valid on restore
            // so we just record the recoveryUnit to make sure
            _savedRecoveryUnit = _txn->recoveryUnit();
            if ( _cursor && !wt_keeptxnopen() ) {
                try {
                    _cursor->reset();
                }
                catch (const WriteConflictException& wce) {
                    // Ignore since this is only called when we are about to kill our transaction
                    // anyway.
                }
            }

            if (_forParallelCollectionScan) {
                // Delete the cursor since we may come back to a different RecoveryUnit
                _cursor.reset();
            }
            _txn = nullptr;
        }

        void saveUnpositioned() final {
            savePositioned();
            _lastReturnedId = RecordId();
        }

        bool restore(OperationContext* txn) final {
            _txn = txn;

            // If we've hit EOF, then this iterator is done and need not be restored.
            if (_eof) return true;

            bool needRestore = false;

            if (_forParallelCollectionScan) {
                needRestore = true;
                _savedRecoveryUnit = txn->recoveryUnit();
                _cursor.reset( new WiredTigerCursor( _rs.getURI(), _rs.instanceId(), true, txn ) );
                _forParallelCollectionScan = false; // we only do this the first time
            }
            invariant( _savedRecoveryUnit == txn->recoveryUnit() );

            if (!needRestore && wt_keeptxnopen()) return true;
            if (_lastReturnedId.isNull()) return true;

            // This will ensure an active session exists, so any restored cursors will bind to it
            invariant(WiredTigerRecoveryUnit::get(txn)->getSession(txn) == _cursor->getSession());

            WT_CURSOR* c = _cursor->get();
            c->set_key(c, _makeKey(_lastReturnedId));

            int cmp;
            int ret = WT_OP_CHECK(c->search_near(c, &cmp));
            if (ret == WT_NOTFOUND) {
                _eof = true;
                return !_rs._isCapped;
            }
            invariantWTOK(ret);

            if (cmp == 0) return true; // Landed right where we left off.

            if (_rs._isCapped) {
                // Doc was deleted either by cappedDeleteAsNeeded() or cappedTruncateAfter().
                // It is important that we error out in this case so that consumers don't
                // silently get 'holes' when scanning capped collections. We don't make 
                // this guarantee for normal collections so it is ok to skip ahead in that case.
                _eof = true;
                return false;
            }

            if (_forward && cmp > 0) {
                // We landed after where we were. Move back one so that next() will return this
                // document.
                ret = WT_OP_CHECK(c->prev(c));
            }
            else if (!_forward && cmp < 0) {
                // Do the opposite for reverse cursors.
                ret = WT_OP_CHECK(c->next(c));
            }
            if (ret != WT_NOTFOUND) invariantWTOK(ret);

            return true;
        }

    private:
        bool isVisible(const RecordId& id) {
            if (!_rs._isCapped) return true;

            if ( _readUntilForOplog.isNull() || !_rs._isOplog ) {
                // this is the normal capped case
                return !_rs.isCappedHidden(id);
            }

            // this is for oplogs
            if (id == _readUntilForOplog) {
                // we allow if its been committed already
                return !_rs.isCappedHidden(id);
            }

            return id < _readUntilForOplog;
        }

        const WiredTigerRecordStore& _rs;
        OperationContext* _txn;
        RecoveryUnit* _savedRecoveryUnit; // only used to sanity check between save/restore.
        const bool _forward;
        bool _forParallelCollectionScan; // This can go away once SERVER-17364 is resolved.
        std::unique_ptr<WiredTigerCursor> _cursor;
        bool _eof = false;
        RecordId _lastReturnedId;
        const RecordId _readUntilForOplog;
    };

    StatusWith<std::string> WiredTigerRecordStore::parseOptionsField(const BSONObj options) {
        StringBuilder ss;
        BSONForEach(elem, options) {
            if (elem.fieldNameStringData() == "configString") {
                if (elem.type() != String) {
                    return StatusWith<std::string>(ErrorCodes::TypeMismatch, str::stream()
                                                   << "storageEngine.wiredTiger.configString "
                                                   << "must be a string. "
                                                   << "Not adding 'configString' value "
                                                   << elem << " to collection configuration");
                }
                ss << elem.valueStringData() << ',';
            }
            else {
                // Return error on first unrecognized field.
                return StatusWith<std::string>(ErrorCodes::InvalidOptions, str::stream()
                                               << '\'' << elem.fieldNameStringData() << '\''
                                               << " is not a supported option in "
                                               << "storageEngine.wiredTiger");
            }
        }
        return StatusWith<std::string>(ss.str());
    }

    // static
    StatusWith<std::string> WiredTigerRecordStore::generateCreateString(
        StringData ns,
        const CollectionOptions& options,
        StringData extraStrings) {

        // Separate out a prefix and suffix in the default string. User configuration will
        // override values in the prefix, but not values in the suffix.
        str::stream ss;
        ss << "type=file,";
        // Setting this larger than 10m can hurt latencies and throughput degradation if this
        // is the oplog.  See SERVER-16247
        ss << "memory_page_max=10m,";
        // Choose a higher split percent, since most usage is append only. Allow some space
        // for workloads where updates increase the size of documents.
        ss << "split_pct=90,";
        ss << "leaf_value_max=64MB,";
        ss << "checksum=on,";
        if (wiredTigerGlobalOptions.useCollectionPrefixCompression) {
            ss << "prefix_compression,";
        }

        ss << "block_compressor=" << wiredTigerGlobalOptions.collectionBlockCompressor << ",";

        ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())->getOpenConfig(ns);

        ss << extraStrings << ",";

        StatusWith<std::string> customOptions =
            parseOptionsField(options.storageEngine.getObjectField(kWiredTigerEngineName));
        if (!customOptions.isOK())
            return customOptions;

        ss << customOptions.getValue();

        if ( NamespaceString::oplog(ns) ) {
            // force file for oplog
            ss << "type=file,";
            // Tune down to 10m.  See SERVER-16247
            ss << "memory_page_max=10m,";
        }

        // WARNING: No user-specified config can appear below this line. These options are required
        // for correct behavior of the server.

        ss << "key_format=q,value_format=u";

        // Record store metadata
        ss << ",app_metadata=(formatVersion=" << kCurrentRecordStoreVersion;
        if (NamespaceString::oplog(ns)) {
            ss << ",oplogKeyExtractionVersion=1";
        }
        ss << ")";

        return StatusWith<std::string>(ss);
    }

    WiredTigerRecordStore::WiredTigerRecordStore(OperationContext* ctx,
                                                 StringData ns,
                                                 StringData uri,
                                                 bool isCapped,
                                                 int64_t cappedMaxSize,
                                                 int64_t cappedMaxDocs,
                                                 CappedDocumentDeleteCallback* cappedDeleteCallback,
                                                 WiredTigerSizeStorer* sizeStorer)
    : RecordStore( ns ),
              _uri( uri.toString() ),
              _instanceId( WiredTigerSession::genCursorId() ),
              _isCapped( isCapped ),
              _isOplog( NamespaceString::oplog( ns ) ),
              _cappedMaxSize( cappedMaxSize ),
              _cappedMaxSizeSlack( std::min(cappedMaxSize/10, int64_t(16*1024*1024)) ),
              _cappedMaxDocs( cappedMaxDocs ),
              _cappedSleep(0),
              _cappedSleepMS(0),
              _cappedDeleteCallback( cappedDeleteCallback ),
              _cappedDeleteCheckCount(0),
              _useOplogHack(shouldUseOplogHack(ctx, _uri)),
              _sizeStorer( sizeStorer ),
              _sizeStorerCounter(0),
              _shuttingDown(false)
    {
        Status versionStatus = WiredTigerUtil::checkApplicationMetadataFormatVersion(
            ctx, uri, kMinimumRecordStoreVersion, kMaximumRecordStoreVersion);
        if (!versionStatus.isOK()) {
            fassertFailedWithStatusNoTrace(28548, versionStatus);
        }

        if (_isCapped) {
            invariant(_cappedMaxSize > 0);
            invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
        }
        else {
            invariant(_cappedMaxSize == -1);
            invariant(_cappedMaxDocs == -1);
        }

        // Find the largest RecordId currently in use and estimate the number of records.
        Cursor cursor(ctx, *this, /*forward=*/false);
        if (auto record = cursor.next()) {
            int64_t max = _makeKey(record->id);
            _oplog_highestSeen = record->id;
            _nextIdNum.store( 1 + max );

            if ( _sizeStorer ) {
                long long numRecords;
                long long dataSize;
                _sizeStorer->loadFromCache( uri, &numRecords, &dataSize );
                _numRecords.store( numRecords );
                _dataSize.store( dataSize );
                _sizeStorer->onCreate( this, numRecords, dataSize );
            }

            if (_sizeStorer == NULL || _numRecords.load() < kCollectionScanOnCreationThreshold) {
                LOG(1) << "doing scan of collection " << ns << " to get info";

                _numRecords.store(0);
                _dataSize.store(0);

                do {
                    _numRecords.fetchAndAdd(1);
                    _dataSize.fetchAndAdd(record->data.size());
                } while ((record = cursor.next()));

                if ( _sizeStorer ) {
                    _sizeStorer->storeToCache( _uri, _numRecords.load(), _dataSize.load() );
                }
            }

        }
        else {
            _dataSize.store(0);
            _numRecords.store(0);
            // Need to start at 1 so we are always higher than RecordId::min()
            _nextIdNum.store( 1 );
            if ( sizeStorer )
                _sizeStorer->onCreate( this, 0, 0 );
        }

        _hasBackgroundThread = WiredTigerKVEngine::initRsOplogBackgroundThread(ns);
    }

    WiredTigerRecordStore::~WiredTigerRecordStore() {
        {
            boost::lock_guard<boost::timed_mutex> lk(_cappedDeleterMutex);
            _shuttingDown = true;
        }

        LOG(1) << "~WiredTigerRecordStore for: " << ns();
        if ( _sizeStorer ) {
            _sizeStorer->onDestroy( this );
        }
    }

    const char* WiredTigerRecordStore::name() const {
        return kWiredTigerEngineName.c_str();
    }

    bool WiredTigerRecordStore::inShutdown() const {
        boost::lock_guard<boost::timed_mutex> lk(_cappedDeleterMutex);
        return _shuttingDown;
    }

    long long WiredTigerRecordStore::dataSize( OperationContext *txn ) const {
        return _dataSize.load();
    }

    long long WiredTigerRecordStore::numRecords( OperationContext *txn ) const {
        return _numRecords.load();
    }

    bool WiredTigerRecordStore::isCapped() const {
        return _isCapped;
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
        WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn);
        StatusWith<int64_t> result = WiredTigerUtil::getStatisticsValueAs<int64_t>(
            session->getSession(),
            "statistics:" + getURI(), "statistics=(size)", WT_STAT_DSRC_BLOCK_SIZE);
        uassertStatusOK(result.getStatus());

        int64_t size = result.getValue();

        if ( size == 0 && _isCapped ) {
            // Many things assume an empty capped collection still takes up space.
            return 1;
        }
        return size;
    }

    // Retrieve the value from a positioned cursor.
    RecordData WiredTigerRecordStore::_getData(const WiredTigerCursor& cursor) const {
        WT_ITEM value;
        int ret = cursor->get_value(cursor.get(), &value);
        invariantWTOK(ret);

        SharedBuffer data = SharedBuffer::allocate(value.size);
        memcpy( data.get(), value.data, value.size );
        return RecordData(data, value.size);
    }

    RecordData WiredTigerRecordStore::dataFor(OperationContext* txn, const RecordId& loc) const {
        // ownership passes to the shared_array created below
        WiredTigerCursor curwrap( _uri, _instanceId, true, txn);
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        c->set_key(c, _makeKey(loc));
        int ret = WT_OP_CHECK(c->search(c));
        massert(28556, "Didn't find RecordId in WiredTigerRecordStore", ret != WT_NOTFOUND);
        invariantWTOK(ret);
        return _getData(curwrap);
    }

    bool WiredTigerRecordStore::findRecord( OperationContext* txn,
                                            const RecordId& loc, RecordData* out ) const {
        WiredTigerCursor curwrap( _uri, _instanceId, true, txn);
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        c->set_key(c, _makeKey(loc));
        int ret = WT_OP_CHECK(c->search(c));
        if (ret == WT_NOTFOUND) {
            return false;
        }
        invariantWTOK(ret);
        *out = _getData(curwrap);
        return true;
    }

    void WiredTigerRecordStore::deleteRecord( OperationContext* txn, const RecordId& loc ) {
        WiredTigerCursor cursor( _uri, _instanceId, true, txn );
        cursor.assertInActiveTxn();
        WT_CURSOR *c = cursor.get();
        c->set_key(c, _makeKey(loc));
        int ret = WT_OP_CHECK(c->search(c));
        invariantWTOK(ret);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariantWTOK(ret);

        int old_length = old_value.size;

        ret = WT_OP_CHECK(c->remove(c));
        invariantWTOK(ret);

        _changeNumRecords(txn, -1);
        _increaseDataSize(txn, -old_length);
    }

    bool WiredTigerRecordStore::cappedAndNeedDelete() const {
        if (!_isCapped)
            return false;

        if (_dataSize.load() >= _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (_numRecords.load() > _cappedMaxDocs))
            return true;

        return false;
    }

    int64_t WiredTigerRecordStore::cappedDeleteAsNeeded(OperationContext* txn,
                                                        const RecordId& justInserted) {

        // We only want to do the checks occasionally as they are expensive.
        // This variable isn't thread safe, but has loose semantics anyway.
        dassert( !_isOplog || _cappedMaxDocs == -1 );

        if (!cappedAndNeedDelete())
            return 0;

        // ensure only one thread at a time can do deletes, otherwise they'll conflict.
        boost::unique_lock<boost::timed_mutex> lock(_cappedDeleterMutex, boost::defer_lock);

        if (_cappedMaxDocs != -1) {
            lock.lock(); // Max docs has to be exact, so have to check every time.
        }
        else if(_hasBackgroundThread) {
            // We are foreground, and there is a background thread,

            // Check if we need some back pressure.
            if ((_dataSize.load() - _cappedMaxSize) < _cappedMaxSizeSlack) {
                return 0;
            }

            // Back pressure needed!
            // We're not actually going to delete anything, but we're going to syncronize
            // on the deleter thread.
            // Don't wait forever: we're in a transaction, we could block eviction.
            if (!lock.try_lock()) {
                Date_t before = Date_t::now();
                (void)lock.timed_lock(boost::posix_time::millisec(200));
                stdx::chrono::milliseconds delay = Date_t::now() - before;
                _cappedSleep.fetchAndAdd(1);
                _cappedSleepMS.fetchAndAdd(delay.count());
            }
            return 0;
        }
        else {
            if (!lock.try_lock()) {
                // Someone else is deleting old records. Apply back-pressure if too far behind,
                // otherwise continue.
                if ((_dataSize.load() - _cappedMaxSize) < _cappedMaxSizeSlack)
                    return 0;

                // Don't wait forever: we're in a transaction, we could block eviction.
                Date_t before = Date_t::now();
                bool gotLock = lock.timed_lock(boost::posix_time::millisec(200));
                stdx::chrono::milliseconds delay = Date_t::now() - before;
                _cappedSleep.fetchAndAdd(1);
                _cappedSleepMS.fetchAndAdd(delay.count());
                if (!gotLock)
                    return 0;
                
                // If we already waited, let someone else do cleanup unless we are significantly
                // over the limit.
                if ((_dataSize.load() - _cappedMaxSize) < (2 * _cappedMaxSizeSlack))
                    return 0;
            }
        }

        return cappedDeleteAsNeeded_inlock(txn, justInserted);
    }

    int64_t WiredTigerRecordStore::cappedDeleteAsNeeded_inlock(OperationContext* txn,
                                                               const RecordId& justInserted) {
        // we do this is a side transaction in case it aborts
        WiredTigerRecoveryUnit* realRecoveryUnit =
            checked_cast<WiredTigerRecoveryUnit*>( txn->releaseRecoveryUnit() );
        invariant( realRecoveryUnit );
        WiredTigerSessionCache* sc = realRecoveryUnit->getSessionCache();
        OperationContext::RecoveryUnitState const realRUstate =
            txn->setRecoveryUnit(new WiredTigerRecoveryUnit(sc),
                                 OperationContext::kNotInUnitOfWork);

        WiredTigerRecoveryUnit::get(txn)->markNoTicketRequired(); // realRecoveryUnit already has
        WT_SESSION* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn)->getSession();

        int64_t dataSize = _dataSize.load();
        int64_t numRecords = _numRecords.load();

        int64_t sizeOverCap = (dataSize > _cappedMaxSize) ? dataSize - _cappedMaxSize : 0;
        int64_t sizeSaved = 0;
        int64_t docsOverCap = 0, docsRemoved = 0;
        if (_cappedMaxDocs != -1 && numRecords > _cappedMaxDocs)
            docsOverCap = numRecords - _cappedMaxDocs;

        try {
            WriteUnitOfWork wuow(txn);

            WiredTigerCursor curwrap( _uri, _instanceId, true, txn);
            WT_CURSOR *c = curwrap.get();
            RecordId newestOld;
            int ret = 0;
            while ((sizeSaved < sizeOverCap || docsRemoved < docsOverCap) &&
                   (docsRemoved < 20000) &&
                   (ret = WT_OP_CHECK(c->next(c))) == 0) {

                int64_t key;
                ret = c->get_key(c, &key);
                invariantWTOK(ret);

                // don't go past the record we just inserted
                newestOld = _fromKey(key);
                if ( newestOld >= justInserted ) // TODO: use oldest uncommitted instead
                    break;

                if ( _shuttingDown )
                    break;

                WT_ITEM old_value;
                invariantWTOK(c->get_value(c, &old_value));

                ++docsRemoved;
                sizeSaved += old_value.size;

                if ( _cappedDeleteCallback ) {
                    uassertStatusOK(
                        _cappedDeleteCallback->aboutToDeleteCapped(
                            txn,
                            newestOld,
                            RecordData(static_cast<const char*>(old_value.data), old_value.size)));
                }
            }

            if (ret != WT_NOTFOUND) {
                invariantWTOK(ret);
            }

            if (docsRemoved > 0) {
                // if we scanned to the end of the collection or past our insert, go back one
                if (ret == WT_NOTFOUND || newestOld >= justInserted) {
                    ret = WT_OP_CHECK(c->prev(c));
                }
                invariantWTOK(ret);

                WiredTigerCursor startWrap( _uri, _instanceId, true, txn);
                WT_CURSOR* start = startWrap.get();
                ret = WT_OP_CHECK(start->next(start));
                invariantWTOK(ret);

                ret = session->truncate(session, NULL, start, c, NULL);
                if (ret == ENOENT || ret == WT_NOTFOUND) {
                    // TODO we should remove this case once SERVER-17141 is resolved
                    log() << "Soft failure truncating capped collection. Will try again later.";
                    docsRemoved = 0;
                }
                else {
                    invariantWTOK(ret);
                    _changeNumRecords(txn, -docsRemoved);
                    _increaseDataSize(txn, -sizeSaved);
                    wuow.commit();
                }
            }
        }
        catch ( const WriteConflictException& wce ) {
            delete txn->releaseRecoveryUnit();
            txn->setRecoveryUnit(realRecoveryUnit, realRUstate);
            log() << "got conflict truncating capped, ignoring";
            return 0;
        }
        catch ( ... ) {
            delete txn->releaseRecoveryUnit();
            txn->setRecoveryUnit(realRecoveryUnit, realRUstate);
            throw;
        }

        delete txn->releaseRecoveryUnit();
        txn->setRecoveryUnit(realRecoveryUnit, realRUstate);
        return docsRemoved;
    }

    StatusWith<RecordId> WiredTigerRecordStore::extractAndCheckLocForOplog(const char* data,
                                                                           int len) {
        return oploghack::extractKey(data, len);
    }

    StatusWith<RecordId> WiredTigerRecordStore::insertRecord( OperationContext* txn,
                                                              const char* data,
                                                              int len,
                                                              bool enforceQuota ) {
        if ( _isCapped && len > _cappedMaxSize ) {
            return StatusWith<RecordId>( ErrorCodes::BadValue,
                                         "object to insert exceeds cappedMaxSize" );
        }

        RecordId loc;
        if ( _useOplogHack ) {
            StatusWith<RecordId> status = extractAndCheckLocForOplog(data, len);
            if (!status.isOK())
                return status;
            loc = status.getValue();
            if ( loc > _oplog_highestSeen ) {
                boost::lock_guard<boost::mutex> lk( _uncommittedDiskLocsMutex );
                if ( loc > _oplog_highestSeen ) {
                    _oplog_highestSeen = loc;
                }
            }
        }
        else if ( _isCapped ) {
            boost::lock_guard<boost::mutex> lk( _uncommittedDiskLocsMutex );
            loc = _nextId();
            _addUncommitedDiskLoc_inlock( txn, loc );
        }
        else {
            loc = _nextId();
        }

        WiredTigerCursor curwrap( _uri, _instanceId, true, txn);
        curwrap.assertInActiveTxn();
        WT_CURSOR *c = curwrap.get();
        invariant( c );

        c->set_key(c, _makeKey(loc));
        WiredTigerItem value(data, len);
        c->set_value(c, value.Get());
        int ret = WT_OP_CHECK(c->insert(c));
        if (ret) {
            return StatusWith<RecordId>(wtRCToStatus(ret, "WiredTigerRecordStore::insertRecord"));
        }

        _changeNumRecords( txn, 1 );
        _increaseDataSize( txn, len );

        cappedDeleteAsNeeded(txn, loc);

        return StatusWith<RecordId>( loc );
    }

    void WiredTigerRecordStore::dealtWithCappedLoc( const RecordId& loc ) {
        boost::lock_guard<boost::mutex> lk( _uncommittedDiskLocsMutex );
        SortedDiskLocs::iterator it = std::find(_uncommittedDiskLocs.begin(),
                                                _uncommittedDiskLocs.end(),
                                                loc);
        invariant(it != _uncommittedDiskLocs.end());
        _uncommittedDiskLocs.erase(it);
    }

    bool WiredTigerRecordStore::isCappedHidden( const RecordId& loc ) const {
        boost::lock_guard<boost::mutex> lk( _uncommittedDiskLocsMutex );
        if (_uncommittedDiskLocs.empty()) {
            return false;
        }
        return _uncommittedDiskLocs.front() <= loc;
    }

    StatusWith<RecordId> WiredTigerRecordStore::insertRecord( OperationContext* txn,
                                                              const DocWriter* doc,
                                                              bool enforceQuota ) {
        const int len = doc->documentSize();

        boost::shared_array<char> buf( new char[len] );
        doc->writeDocument( buf.get() );

        return insertRecord( txn, buf.get(), len, enforceQuota );
    }

    StatusWith<RecordId> WiredTigerRecordStore::updateRecord( OperationContext* txn,
                                                              const RecordId& loc,
                                                              const char* data,
                                                              int len,
                                                              bool enforceQuota,
                                                              UpdateNotifier* notifier ) {
        WiredTigerCursor curwrap( _uri, _instanceId, true, txn);
        curwrap.assertInActiveTxn();
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        c->set_key(c, _makeKey(loc));
        int ret = WT_OP_CHECK(c->search(c));
        invariantWTOK(ret);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariantWTOK(ret);

        int old_length = old_value.size;

        c->set_key(c, _makeKey(loc));
        WiredTigerItem value(data, len);
        c->set_value(c, value.Get());
        ret = WT_OP_CHECK(c->insert(c));
        invariantWTOK(ret);

        _increaseDataSize(txn, len - old_length);

        cappedDeleteAsNeeded(txn, loc);

        return StatusWith<RecordId>( loc );
    }

    bool WiredTigerRecordStore::updateWithDamagesSupported() const {
        return false;
    }

    Status WiredTigerRecordStore::updateWithDamages( OperationContext* txn,
                                                     const RecordId& loc,
                                                     const RecordData& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages ) {
        invariant(false);
    }

    void WiredTigerRecordStore::_oplogSetStartHack( WiredTigerRecoveryUnit* wru ) const {
        boost::lock_guard<boost::mutex> lk( _uncommittedDiskLocsMutex );
        if ( _uncommittedDiskLocs.empty() ) {
            wru->setOplogReadTill( _oplog_highestSeen );
        }
        else {
            wru->setOplogReadTill( _uncommittedDiskLocs.front() );
        }
    }

    std::unique_ptr<RecordCursor> WiredTigerRecordStore::getCursor(OperationContext* txn,
                                                                   bool forward) const {

        if ( _isOplog && forward ) {
            WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(txn);
            if ( !wru->inActiveTxn() || wru->getOplogReadTill().isNull() ) {
                // if we don't have a session, we have no snapshot, so we can update our view
                _oplogSetStartHack( wru );
            }
        }

        return stdx::make_unique<Cursor>(txn, *this, forward);
    }

    std::vector<std::unique_ptr<RecordCursor>> WiredTigerRecordStore::getManyCursors(
            OperationContext* txn) const {
        std::vector<std::unique_ptr<RecordCursor>> cursors(1);
        cursors[0] = stdx::make_unique<Cursor>(txn, *this, /*forward=*/true,
                                               /*forParallelCollectionScan=*/true);
        return cursors;
    }

    Status WiredTigerRecordStore::truncate( OperationContext* txn ) {
        WiredTigerCursor startWrap( _uri, _instanceId, true, txn);
        WT_CURSOR* start = startWrap.get();
        int ret = WT_OP_CHECK(start->next(start));
        //Empty collections don't have anything to truncate.
        if (ret == WT_NOTFOUND) {
            return Status::OK();
        }
        invariantWTOK(ret);

        WT_SESSION* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn)->getSession();
        invariantWTOK(WT_OP_CHECK(session->truncate(session, NULL, start, NULL, NULL)));
        _changeNumRecords(txn, -numRecords(txn));
        _increaseDataSize(txn, -dataSize(txn));

        return Status::OK();
    }

    Status WiredTigerRecordStore::compact( OperationContext* txn,
                                           RecordStoreCompactAdaptor* adaptor,
                                           const CompactOptions* options,
                                           CompactStats* stats ) {
        WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(txn)->getSessionCache();
        WiredTigerSession* session = cache->getSession();
        WT_SESSION *s = session->getSession();
        int ret = s->compact(s, getURI().c_str(), "timeout=0");
        invariantWTOK(ret);
        cache->releaseSession(session);
        return Status::OK();
    }

    Status WiredTigerRecordStore::validate( OperationContext* txn,
                                            bool full,
                                            bool scanData,
                                            ValidateAdaptor* adaptor,
                                            ValidateResults* results,
                                            BSONObjBuilder* output ) {

        {
            int err = WiredTigerUtil::verifyTable(txn, _uri, &results->errors);
            if (err == EBUSY) {
                const char* msg = "verify() returned EBUSY. Not treating as invalid.";
                warning() << msg;
                results->errors.push_back(msg);
            }
            else if (err) {
                std::string msg = str::stream()
                    << "verify() returned " << wiredtiger_strerror(err) << ". "
                    << "This indicates structural damage. "
                    << "Not examining individual documents.";
                error() << msg;
                results->errors.push_back(msg);
                results->valid = false;
                return Status::OK();
            }
        }

        long long nrecords = 0;
        long long dataSizeTotal = 0;
        results->valid = true;
        Cursor cursor(txn, *this, true);
        while (auto record = cursor.next()) {
            ++nrecords;
            if ( full && scanData ) {
                size_t dataSize;
                Status status = adaptor->validate( record->data, &dataSize );
                if ( !status.isOK() ) {
                    results->valid = false;
                    results->errors.push_back( str::stream() << record->id << " is corrupted" );
                }
                dataSizeTotal += static_cast<long long>(dataSize);
            }
        }

        if (_sizeStorer && full && scanData && results->valid) {
            if (nrecords != _numRecords.load() || dataSizeTotal != _dataSize.load()) {
                warning() << _uri << ": Existing record and data size counters ("
                          << _numRecords.load() << " records " << _dataSize.load() << " bytes) "
                          << "are inconsistent with full validation results ("
                          << nrecords << " records " << dataSizeTotal << " bytes). "
                          << "Updating counters with new values.";
            }

            _numRecords.store(nrecords);
            _dataSize.store(dataSizeTotal);

            long long oldNumRecords;
            long long oldDataSize;
            _sizeStorer->loadFromCache(_uri, &oldNumRecords, &oldDataSize);
            if (nrecords != oldNumRecords || dataSizeTotal != oldDataSize) {
                warning() << _uri << ": Existing data in size storer ("
                          << oldNumRecords << " records " << oldDataSize << " bytes) "
                          << "is inconsistent with full validation results ("
                          << _numRecords.load() << " records " << _dataSize.load() << " bytes). "
                          << "Updating size storer with new values.";
            }

            _sizeStorer->storeToCache(_uri, _numRecords.load(), _dataSize.load());
        }

        output->appendNumber( "nrecords", nrecords );
        return Status::OK();
    }

    void WiredTigerRecordStore::appendCustomStats( OperationContext* txn,
                                                   BSONObjBuilder* result,
                                                   double scale ) const {
        result->appendBool( "capped", _isCapped );
        if ( _isCapped ) {
            result->appendIntOrLL("max", _cappedMaxDocs );
            result->appendIntOrLL("maxSize", static_cast<long long>(_cappedMaxSize / scale) );
            result->appendIntOrLL("sleepCount", _cappedSleep.load());
            result->appendIntOrLL("sleepMS", _cappedSleepMS.load());
        }
        WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn);
        WT_SESSION* s = session->getSession();
        BSONObjBuilder bob(result->subobjStart(kWiredTigerEngineName));
        {
            BSONObjBuilder metadata(bob.subobjStart("metadata"));
            Status status = WiredTigerUtil::getApplicationMetadata(txn, getURI(), &metadata);
            if (!status.isOK()) {
                metadata.append("error", "unable to retrieve metadata");
                metadata.append("code", static_cast<int>(status.code()));
                metadata.append("reason", status.reason());
            }
        }

        std::string type, sourceURI;
        WiredTigerUtil::fetchTypeAndSourceURI(txn, _uri, &type, &sourceURI);
        StatusWith<std::string> metadataResult = WiredTigerUtil::getMetadata(txn, sourceURI);
        StringData creationStringName("creationString");
        if (!metadataResult.isOK()) {
            BSONObjBuilder creationString(bob.subobjStart(creationStringName));
            creationString.append("error", "unable to retrieve creation config");
            creationString.append("code", static_cast<int>(metadataResult.getStatus().code()));
            creationString.append("reason", metadataResult.getStatus().reason());
        }
        else {
            bob.append("creationString", metadataResult.getValue());
            // Type can be "lsm" or "file"
            bob.append("type", type);
        }

        Status status = WiredTigerUtil::exportTableToBSON(s, "statistics:" + getURI(),
                                                          "statistics=(fast)", &bob);
        if (!status.isOK()) {
            bob.append("error", "unable to retrieve statistics");
            bob.append("code", static_cast<int>(status.code()));
            bob.append("reason", status.reason());
        }

    }

    Status WiredTigerRecordStore::oplogDiskLocRegister( OperationContext* txn,
                                                        const Timestamp& opTime ) {
        StatusWith<RecordId> loc = oploghack::keyForOptime( opTime );
        if ( !loc.isOK() )
            return loc.getStatus();

        boost::lock_guard<boost::mutex> lk( _uncommittedDiskLocsMutex );
        _addUncommitedDiskLoc_inlock( txn, loc.getValue() );
        return Status::OK();
    }

    class WiredTigerRecordStore::CappedInsertChange : public RecoveryUnit::Change {
    public:
        CappedInsertChange( WiredTigerRecordStore* rs, const RecordId& loc )
            : _rs( rs ), _loc( loc ) {
        }

        virtual void commit() {
            _rs->dealtWithCappedLoc( _loc );
        }

        virtual void rollback() {
            _rs->dealtWithCappedLoc( _loc );
        }

    private:
        WiredTigerRecordStore* _rs;
        RecordId _loc;
    };

    void WiredTigerRecordStore::_addUncommitedDiskLoc_inlock( OperationContext* txn,
                                                              const RecordId& loc ) {
        // todo: make this a dassert at some point
        invariant( _uncommittedDiskLocs.empty() ||
                   _uncommittedDiskLocs.back() < loc );
        _uncommittedDiskLocs.push_back( loc );
        txn->recoveryUnit()->registerChange( new CappedInsertChange( this, loc ) );
        _oplog_highestSeen = loc;
    }

    boost::optional<RecordId> WiredTigerRecordStore::oplogStartHack(
            OperationContext* txn,
            const RecordId& startingPosition) const {

        if (!_useOplogHack)
            return boost::none;

        {
            WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(txn);
            _oplogSetStartHack( wru );
        }

        WiredTigerCursor cursor(_uri, _instanceId, true, txn);
        WT_CURSOR* c = cursor.get();

        int cmp;
        c->set_key(c, _makeKey(startingPosition));
        int ret = WT_OP_CHECK(c->search_near(c, &cmp));
        if (ret == 0 && cmp > 0) ret = c->prev(c); // landed one higher than startingPosition
        if (ret == WT_NOTFOUND) return RecordId(); // nothing <= startingPosition
        invariantWTOK(ret);

        int64_t key;
        ret = c->get_key(c, &key);
        invariantWTOK(ret);
        return _fromKey(key);
    }

    void WiredTigerRecordStore::updateStatsAfterRepair(OperationContext* txn,
                                                       long long numRecords,
                                                       long long dataSize) {
        _numRecords.store(numRecords);
        _dataSize.store(dataSize);
        _sizeStorer->storeToCache(_uri, numRecords, dataSize);
    }

    RecordId WiredTigerRecordStore::_nextId() {
        invariant(!_useOplogHack);
        RecordId out = RecordId(_nextIdNum.fetchAndAdd(1));
        invariant(out.isNormal());
        return out;
    }

    WiredTigerRecoveryUnit* WiredTigerRecordStore::_getRecoveryUnit( OperationContext* txn ) {
        return checked_cast<WiredTigerRecoveryUnit*>( txn->recoveryUnit() );
    }

    class WiredTigerRecordStore::NumRecordsChange : public RecoveryUnit::Change {
    public:
        NumRecordsChange(WiredTigerRecordStore* rs, int64_t diff) :_rs(rs), _diff(diff) {}
        virtual void commit() {}
        virtual void rollback() {
            _rs->_numRecords.fetchAndAdd( -_diff );
        }

    private:
        WiredTigerRecordStore* _rs;
        int64_t _diff;
    };

    void WiredTigerRecordStore::_changeNumRecords( OperationContext* txn, int64_t diff ) {
        txn->recoveryUnit()->registerChange(new NumRecordsChange(this, diff));
        if ( diff > 0 ) {
            if ( _numRecords.fetchAndAdd( diff ) < diff )
                    _numRecords.store( diff );
        } else if ( _numRecords.fetchAndAdd( diff ) < 0 ) {
            _numRecords.store( 0 );
        }
    }

    class WiredTigerRecordStore::DataSizeChange : public RecoveryUnit::Change {
    public:
        DataSizeChange(WiredTigerRecordStore* rs, int amount) :_rs(rs), _amount(amount) {}
        virtual void commit() {}
        virtual void rollback() {
            _rs->_increaseDataSize( NULL, -_amount );
        }

    private:
        WiredTigerRecordStore* _rs;
        bool _amount;
    };

    void WiredTigerRecordStore::_increaseDataSize( OperationContext* txn, int amount ) {
        if ( txn )
            txn->recoveryUnit()->registerChange(new DataSizeChange(this, amount));

        if ( _dataSize.fetchAndAdd(amount) < 0 ) {
            if ( amount > 0 ) {
                _dataSize.store( amount );
            }
            else {
                _dataSize.store( 0 );
            }
        }

        if ( _sizeStorer && _sizeStorerCounter++ % 1000 == 0 ) {
            _sizeStorer->storeToCache( _uri, _numRecords.load(), _dataSize.load() );
        }
    }

    int64_t WiredTigerRecordStore::_makeKey( const RecordId& loc ) {
        return loc.repr();
    }
    RecordId WiredTigerRecordStore::_fromKey( int64_t key ) {
        return RecordId(key);
    }

    void WiredTigerRecordStore::temp_cappedTruncateAfter( OperationContext* txn,
                                                          RecordId end,
                                                          bool inclusive ) {
        WriteUnitOfWork wuow(txn);
        Cursor cursor(txn, *this);
        while (auto record = cursor.next()) {
            RecordId loc = record->id;
            if ( end < loc || ( inclusive && end == loc ) ) {
                deleteRecord( txn, loc );
            }
        }
        wuow.commit();
    }
}
