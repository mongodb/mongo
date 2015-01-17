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

#include <boost/scoped_ptr.hpp>
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
                            bool durable = true,
                            bool repair = false );
        virtual ~WiredTigerKVEngine();

        void setRecordStoreExtraOptions( const std::string& options );
        void setSortedDataInterfaceExtraOptions( const std::string& options );

        virtual bool supportsDocLocking() const;

        virtual bool supportsDirectoryPerDB() const;

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

        virtual Status createSortedDataInterface( OperationContext* opCtx,
                                                  const StringData& ident,
                                                  const IndexDescriptor* desc );

        virtual SortedDataInterface* getSortedDataInterface( OperationContext* opCtx,
                                                             const StringData& ident,
                                                             const IndexDescriptor* desc );

        virtual Status dropIdent( OperationContext* opCtx,
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

        virtual bool hasIdent(OperationContext* opCtx, const StringData& ident) const;

        std::vector<std::string> getAllIdents( OperationContext* opCtx ) const;

        virtual void cleanShutdown();

        // wiredtiger specific
        // Calls WT_CONNECTION::reconfigure on the underlying WT_CONNECTION
        // held by this class
        int reconfigure(const char* str);

        WT_CONNECTION* getConnection() { return _conn; }
        void dropAllQueued();
        bool haveDropsQueued() const;

        void syncSizeInfo(bool sync) const;

    private:

        Status _salvageIfNeeded(const char* uri);
        void _checkIdentPath( const StringData& ident );

        bool _hasUri(WT_SESSION* session, const std::string& uri) const;

        std::string _uri( const StringData& ident ) const;
        bool _drop( const StringData& ident );

        WT_CONNECTION* _conn;
        WT_EVENT_HANDLER _eventHandler;
        boost::scoped_ptr<WiredTigerSessionCache> _sessionCache;
        std::string _path;
        bool _durable;

        std::string _rsOptions;
        std::string _indexOptions;

        std::set<std::string> _identToDrop;
        mutable boost::mutex _identToDropMutex;

        boost::scoped_ptr<WiredTigerSizeStorer> _sizeStorer;
        std::string _sizeStorerUri;
        mutable ElapsedTracker _sizeStorerSyncTracker;
    };

}
