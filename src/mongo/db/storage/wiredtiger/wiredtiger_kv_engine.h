// wiredtiger_kv_engine.h

#pragma once

#include <wiredtiger.h>

#include "mongo/bson/ordering.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"

namespace mongo {

    class WiredTigerSessionCache;

    class WiredTigerKVEngine : public KVEngine {
    public:
        WiredTigerKVEngine( const std::string& path,
                            const std::string& extraOpenOptions = "" );
        virtual ~WiredTigerKVEngine();

        void setRecordStoreExtraOptions( const std::string& options );
        void setSortedDataInterfaceExtraOptions( const std::string& options );

        virtual RecoveryUnit* newRecoveryUnit();

        virtual Status createRecordStore( OperationContext* opCtx,
                                          const StringData& ns,
                                          const StringData& ident,
                                          const CollectionOptions& options );

        virtual RecordStore* getRecordStore( OperationContext* opCtx,
                                             const StringData& ns,
                                             const StringData& ident,
                                             const CollectionOptions& options );

        virtual Status dropRecordStore( OperationContext* opCtx,
                                        const StringData& ident );

        virtual Status createSortedDataInterface( OperationContext* opCtx,
                                                  const StringData& ident,
                                                  const IndexDescriptor* desc );

        virtual SortedDataInterface* getSortedDataInterface( OperationContext* opCtx,
                                                             const StringData& ident,
                                                             const IndexDescriptor* desc );

        virtual Status dropSortedDataInterface( OperationContext* opCtx,
                                                const StringData& ident );

    private:

        string _uri( const StringData& ident ) const;

        WT_CONNECTION* _conn;
        WT_EVENT_HANDLER _eventHandler;
        boost::scoped_ptr<WiredTigerSessionCache> _sessionCache;

        string _rsOptions;
        string _indexOptions;

    };

}
