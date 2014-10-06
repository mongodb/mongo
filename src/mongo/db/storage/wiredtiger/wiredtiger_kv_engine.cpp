// wiredtiger_kv_engine.cpp

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"

namespace mongo {

    WiredTigerKVEngine::WiredTigerKVEngine( const std::string& path,
                                            const std::string& extraOpenOptions ) {
        std::stringstream ss;
        ss << "create,";
        ss << "cache_size=1G,";
        ss << "extensions=[local=(entry=index_collator_extension)],";
        ss << "log=(enabled),";
        ss << extraOpenOptions;
        string config = ss.str();
        invariantWTOK(wiredtiger_open(path.c_str(), NULL, config.c_str(), &_conn));

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
        return new WiredTigerRecoveryUnit( _sessionCache.get(), false );
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
                                                                     const IndexDescriptor* dexc ) {
        return new WiredTigerIndex( _uri( ident ) );
    }

    Status WiredTigerKVEngine::dropSortedDataInterface( OperationContext* opCtx,
                                                        const StringData& ident ) {
        // todo: drop not support yet
        log() << "WiredTigerKVEngine::dropSortedDataInterface faking it right now for: " << ident;
        return Status::OK();
    }

}
