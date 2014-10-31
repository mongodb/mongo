// wiredtiger_kv_engine.h

#pragma once

#include <set>
#include <string>

#include <boost/thread/mutex.hpp>

#include <wiredtiger.h>

#include "mongo/bson/ordering.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

    class WiredTigerSessionCache;
    class WiredTigerSizeStorer;

    class WiredTigerKVEngine : public KVEngine {
    public:
        WiredTigerKVEngine( const std::string& path,
                            const std::string& extraOpenOptions = "" );
        virtual ~WiredTigerKVEngine();

        void setRecordStoreExtraOptions( const std::string& options );
        void setSortedDataInterfaceExtraOptions( const std::string& options );

        virtual bool supportsDocLocking() const;

        virtual bool isDurable() const { return true; }

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

        virtual Status okToRename( OperationContext* opCtx,
                                   const StringData& fromNS,
                                   const StringData& toNS,
                                   const StringData& ident,
                                   const RecordStore* originalRecordStore ) const;

        virtual int flushAllFiles( bool sync );

        // wiredtiger specific

        WT_CONNECTION* getConnection() { return _conn; }
        void dropAllQueued();
        bool haveDropsQueued() const;

        int currentEpoch() const { return _epoch; }

        void syncSizeInfo() const;

    private:

        string _uri( const StringData& ident ) const;
        bool _drop( const StringData& ident );

        WT_CONNECTION* _conn;
        WT_EVENT_HANDLER _eventHandler;
        boost::scoped_ptr<WiredTigerSessionCache> _sessionCache;

        string _rsOptions;
        string _indexOptions;

        std::set<std::string> _identToDrop;
        mutable boost::mutex _identToDropMutex;

        int _epoch; // this is how we keep track of if a session is too old

        scoped_ptr<WiredTigerSizeStorer> _sizeStorer;
        string _sizeStorerUri;
        mutable ElapsedTracker _sizeStorerSyncTracker;
    };

}
