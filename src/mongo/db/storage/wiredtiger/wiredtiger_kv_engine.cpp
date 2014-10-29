// wiredtiger_kv_engine.cpp

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"

namespace mongo {

    namespace {
        int mdb_handle_error(WT_EVENT_HANDLER *handler, WT_SESSION *session,
                             int errorCode, const char *message) {
            error() << "WiredTiger (" << errorCode << ") " << message;
            return 0;
        }

        int mdb_handle_message( WT_EVENT_HANDLER *handler, WT_SESSION *session,
                                const char *message) {
            log() << "WiredTiger " << message;
            return 0;
        }

        int mdb_handle_progress( WT_EVENT_HANDLER *handler, WT_SESSION *session,
                                 const char *operation, uint64_t progress) {
            log() << "WiredTiger progress " << operation << " " << progress;
            return 0;
        }

        int mdb_handle_close( WT_EVENT_HANDLER *handler, WT_SESSION *session,
                              WT_CURSOR *cursor) {
            // no-op?
            return 0;
        }

    }

    WiredTigerKVEngine::WiredTigerKVEngine( const std::string& path,
                                            const std::string& extraOpenOptions )
        : _epoch( 0 ),
          _sizeStorerSyncTracker( 100000, 60 * 1000 ) {

        _eventHandler.handle_error = mdb_handle_error;
        _eventHandler.handle_message = mdb_handle_message;
        _eventHandler.handle_progress = mdb_handle_progress;
        _eventHandler.handle_close = mdb_handle_close;

        int cacheSizeGB = 1;

        {
            ProcessInfo pi;
            BSONObjBuilder b;
            pi.appendSystemDetails( b );
            BSONObj obj = b.obj();
            BSONObj extra = obj["extra"].Obj();
            double pageSize = extra["pageSize"].number();
            double numPages = extra["numPages"].number();
            if ( pageSize > 0 && numPages > 0 ) {
                double totalBytes = numPages * pageSize;
                double cacheBytes = totalBytes / 10;
                cacheSizeGB = static_cast<int>( cacheBytes / ( 1024 * 1024 * 1024 ) );
                if ( cacheSizeGB < 1 )
                    cacheSizeGB = 1;
            }
        }

        std::stringstream ss;
        ss << "create,";
        ss << "cache_size=" << cacheSizeGB << "G,";
        ss << "session_max=20000,";
        ss << "extensions=[local=(entry=index_collator_extension)],";
        ss << "statistics=(all),";
        ss << "log=(enabled),";
        ss << extraOpenOptions;
        string config = ss.str();
        LOG(1) << "wiredtiger_open config: " << config;
        invariantWTOK(wiredtiger_open(path.c_str(), &_eventHandler, config.c_str(), &_conn));
        _sessionCache.reset( new WiredTigerSessionCache( this ) );

        _sizeStorerUri = "table:sizeStorer";
        {
            WiredTigerSession session( _conn, -1 );
            WiredTigerSizeStorer* ss = new WiredTigerSizeStorer();
            ss->loadFrom( &session, _sizeStorerUri );
            _sizeStorer.reset( ss );
        }
    }


    WiredTigerKVEngine::~WiredTigerKVEngine() {
        log() << "WiredTigerKVEngine shutting down";
        syncSizeInfo();
        _sizeStorer.reset( NULL );

        _sessionCache.reset( NULL );

        if ( _conn ) {
            invariantWTOK( _conn->close(_conn, NULL) );
            _conn = NULL;
        }

    }

    Status WiredTigerKVEngine::okToRename( OperationContext* opCtx,
                                           const StringData& fromNS,
                                           const StringData& toNS,
                                           const StringData& ident,
                                           const RecordStore* originalRecordStore ) const {
        _sizeStorer->store( _uri( ident ),
                            originalRecordStore->numRecords( opCtx ),
                            originalRecordStore->dataSize( opCtx ) );
        syncSizeInfo();
        return Status::OK();
    }

    int WiredTigerKVEngine::flushAllFiles( bool sync ) {
        LOG(1) << "WiredTigerKVEngine::flushAllFiles";
        syncSizeInfo();
        return 1;
    }

    void WiredTigerKVEngine::syncSizeInfo() const {
        if ( !_sizeStorer )
            return;

        WiredTigerSession session( _conn, -1 );
        WT_SESSION* s = session.getSession();
        invariantWTOK( s->begin_transaction( s, "sync=true" ) );
        _sizeStorer->storeInto( &session, _sizeStorerUri );
        invariantWTOK( s->commit_transaction( s, NULL ) );
    }

    RecoveryUnit* WiredTigerKVEngine::newRecoveryUnit() {
        return new WiredTigerRecoveryUnit( _sessionCache.get() );
    }

    void WiredTigerKVEngine::setRecordStoreExtraOptions( const std::string& options ) {
        _rsOptions = options;
    }

    void WiredTigerKVEngine::setSortedDataInterfaceExtraOptions( const std::string& options ) {
        _indexOptions = options;
    }

    Status WiredTigerKVEngine::createRecordStore( OperationContext* opCtx,
                                                  const StringData& ns,
                                                  const StringData& ident,
                                                  const CollectionOptions& options ) {
        WiredTigerSession session( _conn, -1 );

        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString(ns, options, _rsOptions);
        if (!result.isOK()) {
            return result.getStatus();
        }
        std::string config = result.getValue();

        string uri = _uri( ident );
        WT_SESSION* s = session.getSession();
        LOG(1) << "WiredTigerKVEngine::createRecordStore uri: " << uri << " config: " << config;
        return wtRCToStatus( s->create( s, uri.c_str(), config.c_str() ) );
    }

    RecordStore* WiredTigerKVEngine::getRecordStore( OperationContext* opCtx,
                                                     const StringData& ns,
                                                     const StringData& ident,
                                                     const CollectionOptions& options ) {

        if (options.capped) {
            return new WiredTigerRecordStore(opCtx, ns, _uri(ident), options.capped,
                                             options.cappedSize ? options.cappedSize : 4096,
                                             options.cappedMaxDocs ? options.cappedMaxDocs : -1,
                                             NULL,
                                             _sizeStorer.get() );
        }
        else {
            return new WiredTigerRecordStore(opCtx, ns, _uri(ident),
                                             false, -1, -1, NULL, _sizeStorer.get() );
        }
    }

    Status WiredTigerKVEngine::dropRecordStore( OperationContext* opCtx,
                                                const StringData& ident ) {
        _drop( ident );
        return Status::OK();
    }

    string WiredTigerKVEngine::_uri( const StringData& ident ) const {
        return string("table:") + ident.toString();
    }

    Status WiredTigerKVEngine::createSortedDataInterface( OperationContext* opCtx,
                                                          const StringData& ident,
                                                          const IndexDescriptor* desc ) {
        return wtRCToStatus( WiredTigerIndex::Create( opCtx, _uri( ident ), _indexOptions, desc ) );
    }

    SortedDataInterface* WiredTigerKVEngine::getSortedDataInterface( OperationContext* opCtx,
                                                                     const StringData& ident,
                                                                     const IndexDescriptor* desc ) {
        if ( desc->unique() )
            return new WiredTigerIndexUnique( _uri( ident ) );
        return new WiredTigerIndexStandard( _uri( ident ) );
    }

    Status WiredTigerKVEngine::dropSortedDataInterface( OperationContext* opCtx,
                                                        const StringData& ident ) {
        _drop( ident );
        return Status::OK();
    }

    bool WiredTigerKVEngine::_drop( const StringData& ident ) {
        string uri = _uri( ident );

        WiredTigerSession session( _conn, -1 );

        int ret = session.getSession()->drop( session.getSession(), uri.c_str(), "force" );
        LOG(1) << "WT drop of  " << uri << " res " << ret;

        if ( ret == 0 ) {
            // yay, it worked
            return true;
        }

        if ( ret == EBUSY ) {
            // this is expected, queue it up
            {
                boost::mutex::scoped_lock lk( _identToDropMutex );
                _identToDrop.insert( uri );
                _epoch++;
            }
            _sessionCache->closeAll();
            return false;
        }

        invariantWTOK( ret );
        return false;
    }

    bool WiredTigerKVEngine::haveDropsQueued() const {
        if ( _sizeStorerSyncTracker.intervalHasElapsed() ) {
            _sizeStorerSyncTracker.resetLastTime();
            syncSizeInfo();
        }
        boost::mutex::scoped_lock lk( _identToDropMutex );
        return !_identToDrop.empty();
    }

    void WiredTigerKVEngine::dropAllQueued() {
        set<string> mine;
        {
            boost::mutex::scoped_lock lk( _identToDropMutex );
            mine = _identToDrop;
        }

        set<string> deleted;

        {
            WiredTigerSession session( _conn, -1 );
            for ( set<string>::const_iterator it = mine.begin(); it != mine.end(); ++it ) {
                string uri = *it;
                int ret = session.getSession()->drop( session.getSession(), uri.c_str(), "force" );
                LOG(1) << "WT queued drop of  " << uri << " res " << ret;

                if ( ret == 0 ) {
                    deleted.insert( uri );
                    continue;
                }

                if ( ret == EBUSY ) {
                    // leave in qeuue
                    continue;
                }

                invariantWTOK( ret );
            }
        }

        {
            boost::mutex::scoped_lock lk( _identToDropMutex );
            for ( set<string>::const_iterator it = deleted.begin(); it != deleted.end(); ++it ) {
                _identToDrop.erase( *it );
            }
        }
    }

    bool WiredTigerKVEngine::supportsDocLocking() const {
        return true;
    }

}
