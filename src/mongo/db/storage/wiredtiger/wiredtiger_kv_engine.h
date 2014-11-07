// wiredtiger_kv_engine.h

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
                            const std::string& extraOpenOptions = "",
                            bool durable = true );
        virtual ~WiredTigerKVEngine();

        void setRecordStoreExtraOptions( const std::string& options );
        void setSortedDataInterfaceExtraOptions( const std::string& options );

        virtual bool supportsDocLocking() const;

        virtual bool isDurable() const { return _durable; }

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

        virtual int64_t getIdentSize( OperationContext* opCtx,
                                      const StringData& ident );

        virtual Status repairIdent( OperationContext* opCtx,
                                    const StringData& ident );

        virtual void cleanShutdown(OperationContext* txn);

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
        bool _durable;

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
