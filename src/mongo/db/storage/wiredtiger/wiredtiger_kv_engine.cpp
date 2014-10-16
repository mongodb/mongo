// wiredtiger_kv_engine.cpp

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"

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
                                            const std::string& extraOpenOptions ) {

        _eventHandler.handle_error = mdb_handle_error;
        _eventHandler.handle_message = mdb_handle_message;
        _eventHandler.handle_progress = mdb_handle_progress;
        _eventHandler.handle_close = mdb_handle_close;

        std::stringstream ss;
        ss << "create,";
        ss << "cache_size=1G,";
        ss << "session_max=20000,";
        ss << "extensions=[local=(entry=index_collator_extension)],";
        ss << "statistics=(all),";
        ss << "log=(enabled),";
        ss << extraOpenOptions;
        string config = ss.str();
        invariantWTOK(wiredtiger_open(path.c_str(), &_eventHandler, config.c_str(), &_conn));
        _sessionCache.reset( new WiredTigerSessionCache( _conn ) );
    }


    WiredTigerKVEngine::~WiredTigerKVEngine() {
        _sessionCache.reset( NULL );

        if ( _conn ) {
            invariantWTOK( _conn->close(_conn, NULL) );
            _conn = NULL;
        }

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
                                                  const StringData& ident,
                                                  const CollectionOptions& options ) {
        scoped_ptr<WiredTigerSession> session( _sessionCache->getSession() );

        std::string config = WiredTigerRecordStore::generateCreateString( options, _rsOptions );

        string uri = _uri( ident );
        WT_SESSION* s = session->getSession();
        LOG(1) << "WiredTigerKVEngine::createRecordStore uri: " << uri;
        return wtRCToStatus( s->create( s, uri.c_str(), config.c_str() ) );
    }

    RecordStore* WiredTigerKVEngine::getRecordStore( OperationContext* opCtx,
                                                     const StringData& ns,
                                                     const StringData& ident,
                                                     const CollectionOptions& options ) {

        WiredTigerRecordStore* rs = new WiredTigerRecordStore( opCtx, ns, _uri( ident ) );

        if ( options.capped )
            rs->setCapped(options.cappedSize ? options.cappedSize : 4096,
                          options.cappedMaxDocs ? options.cappedMaxDocs : -1);

        return rs;
    }

    Status WiredTigerKVEngine::dropRecordStore( OperationContext* opCtx,
                                                const StringData& ident ) {
        // todo: drop not support yet
        log() << "WiredTigerKVEngine::dropRecordStore faking it right now for: " << ident;
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
        // todo: drop not support yet
        log() << "WiredTigerKVEngine::dropSortedDataInterface faking it right now for: " << ident;
        return Status::OK();
    }

}
