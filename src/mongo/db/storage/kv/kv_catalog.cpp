// kv_catalog.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
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

#include "mongo/db/storage/kv/kv_catalog.h"

#include <stdlib.h>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/platform/random.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {
    // This is a global resource, which protects accesses to the catalog metadata (instance-wide).
    // It is never used with KVEngines that support doc-level locking so this should never conflict
    // with anything else.
    //
    // NOTE: Must be locked *before* _identLock.
    const ResourceId resourceIdCatalogMetadata(RESOURCE_METADATA, 1ULL);
}

    using std::unique_ptr;
    using std::string;

    class KVCatalog::AddIdentChange : public RecoveryUnit::Change {
    public:
        AddIdentChange(KVCatalog* catalog, StringData ident)
            :_catalog(catalog), _ident(ident.toString())
        {}

        virtual void commit() {}
        virtual void rollback() {
            boost::lock_guard<boost::mutex> lk(_catalog->_identsLock);
            _catalog->_idents.erase(_ident);
        }

        KVCatalog* const _catalog;
        const std::string _ident;
    };

    class KVCatalog::RemoveIdentChange : public RecoveryUnit::Change {
    public:
        RemoveIdentChange(KVCatalog* catalog, StringData ident, const Entry& entry)
            :_catalog(catalog), _ident(ident.toString()), _entry(entry)
        {}

        virtual void commit() {}
        virtual void rollback() {
            boost::lock_guard<boost::mutex> lk(_catalog->_identsLock);
            _catalog->_idents[_ident] = _entry;
        }

        KVCatalog* const _catalog;
        const std::string _ident;
        const Entry _entry;
    };

    KVCatalog::KVCatalog( RecordStore* rs,
                          bool isRsThreadSafe,
                          bool directoryPerDb,
                          bool directoryForIndexes )
        : _rs( rs )
        , _isRsThreadSafe(isRsThreadSafe)
        , _directoryPerDb(directoryPerDb)
        , _directoryForIndexes(directoryForIndexes)
        , _rand(_newRand())
    {}

    KVCatalog::~KVCatalog() {
        _rs = NULL;
    }

    std::string KVCatalog::_newRand() {
        return str::stream()
            << std::unique_ptr<SecureRandom>(SecureRandom::create())->nextInt64();
    }

    bool KVCatalog::_hasEntryCollidingWithRand() const {
        // Only called from init() so don't need to lock.
        for (NSToIdentMap::const_iterator it = _idents.begin(); it != _idents.end(); ++it) {
            if (StringData(it->first).endsWith(_rand))
                return true;
        }
        return false;
    }

    std::string KVCatalog::_newUniqueIdent(StringData ns, const char* kind) {
        // If this changes to not put _rand at the end, _hasEntryCollidingWithRand will need fixing.
        StringBuilder buf;
        if ( _directoryPerDb ) {
            buf << NamespaceString::escapeDbName( nsToDatabaseSubstring( ns ) ) << '/';
        }
        buf << kind;
        buf << ( _directoryForIndexes ? '/' : '-' );
        buf << _next.fetchAndAdd(1) << '-' << _rand;
        return buf.str();
    }

    void KVCatalog::init( OperationContext* opCtx ) {
        // No locking needed since called single threaded.
        auto cursor = _rs->getCursor(opCtx);
        while (auto record = cursor->next()) {
            BSONObj obj = record->data.releaseToBson();

            // No rollback since this is just loading already committed data.
            string ns = obj["ns"].String();
            string ident = obj["ident"].String();
            _idents[ns] = Entry(ident, record->id);
        }

        // In the unlikely event that we have used this _rand before generate a new one.
        while (_hasEntryCollidingWithRand()) {
            _rand = _newRand();
        }
    }

    void KVCatalog::getAllCollections( std::vector<std::string>* out ) const {
        boost::lock_guard<boost::mutex> lk( _identsLock );
        for ( NSToIdentMap::const_iterator it = _idents.begin(); it != _idents.end(); ++it ) {
            out->push_back( it->first );
        }
    }

    Status KVCatalog::newCollection( OperationContext* opCtx,
                                     StringData ns,
                                     const CollectionOptions& options ) {
        invariant( opCtx->lockState() == NULL ||
                   opCtx->lockState()->isDbLockedForMode( nsToDatabaseSubstring(ns), MODE_X ) );

        std::unique_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe && opCtx->lockState()) {
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(),
                                             resourceIdCatalogMetadata,
                                             MODE_X));
        }

        const string ident = _newUniqueIdent(ns, "collection");

        boost::lock_guard<boost::mutex> lk( _identsLock );
        Entry& old = _idents[ns.toString()];
        if ( !old.ident.empty() ) {
            return Status( ErrorCodes::NamespaceExists, "collection already exists" );
        }

        opCtx->recoveryUnit()->registerChange(new AddIdentChange(this, ns));

        BSONObj obj;
        {
            BSONObjBuilder b;
            b.append( "ns", ns );
            b.append( "ident", ident );
            BSONCollectionCatalogEntry::MetaData md;
            md.ns = ns.toString();
            md.options = options;
            b.append( "md", md.toBSON() );
            obj = b.obj();
        }

        StatusWith<RecordId> res = _rs->insertRecord( opCtx, obj.objdata(), obj.objsize(), false );
        if ( !res.isOK() )
            return res.getStatus();

        old = Entry( ident, res.getValue() );
        LOG(1) << "stored meta data for " << ns << " @ " << res.getValue();
        return Status::OK();
    }

    std::string KVCatalog::getCollectionIdent( StringData ns ) const {
        boost::lock_guard<boost::mutex> lk( _identsLock );
        NSToIdentMap::const_iterator it = _idents.find( ns.toString() );
        invariant( it != _idents.end() );
        return it->second.ident;
    }

    std::string KVCatalog::getIndexIdent( OperationContext* opCtx,
                                          StringData ns,
                                          StringData idxName ) const {
        BSONObj obj = _findEntry( opCtx, ns );
        BSONObj idxIdent = obj["idxIdent"].Obj();
        return idxIdent[idxName].String();
    }

    BSONObj KVCatalog::_findEntry( OperationContext* opCtx,
                                   StringData ns,
                                   RecordId* out ) const {

        std::unique_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe && opCtx->lockState()) {
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(),
                                             resourceIdCatalogMetadata,
                                             MODE_S));
        }

        RecordId dl;
        {
            boost::lock_guard<boost::mutex> lk( _identsLock );
            NSToIdentMap::const_iterator it = _idents.find( ns.toString() );
            invariant( it != _idents.end() );
            dl = it->second.storedLoc;
        }

        LOG(1) << "looking up metadata for: " << ns << " @ " << dl;
        RecordData data;
        if ( !_rs->findRecord( opCtx, dl, &data ) ) {
            // since the in memory meta data isn't managed with mvcc
            // its possible for different transactions to see slightly
            // different things, which is ok via the locking above.
            return BSONObj();
        }

        if (out)
            *out = dl;

        return data.releaseToBson().getOwned();
    }

    const BSONCollectionCatalogEntry::MetaData KVCatalog::getMetaData( OperationContext* opCtx,
                                                                       StringData ns ) {
        BSONObj obj = _findEntry( opCtx, ns );
        LOG(3) << " fetched CCE metadata: " << obj;
        BSONCollectionCatalogEntry::MetaData md;
        const BSONElement mdElement = obj["md"];
        if ( mdElement.isABSONObj() ) {
            LOG(3) << "returning metadata: " << mdElement;
            md.parse( mdElement.Obj() );
        }
        return md;
    }

    void KVCatalog::putMetaData( OperationContext* opCtx,
                                 StringData ns,
                                 BSONCollectionCatalogEntry::MetaData& md ) {

        std::unique_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe && opCtx->lockState()) {
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(),
                                             resourceIdCatalogMetadata,
                                             MODE_X));
        }

        RecordId loc;
        BSONObj obj = _findEntry( opCtx, ns, &loc );

        {
            // rebuilt doc
            BSONObjBuilder b;
            b.append( "md", md.toBSON() );

            BSONObjBuilder newIdentMap;
            BSONObj oldIdentMap;
            if ( obj["idxIdent"].isABSONObj() )
                oldIdentMap = obj["idxIdent"].Obj();

            // fix ident map
            for ( size_t i = 0; i < md.indexes.size(); i++ ) {
                string name = md.indexes[i].name();
                BSONElement e = oldIdentMap[name];
                if ( e.type() == String ) {
                    newIdentMap.append( e );
                    continue;
                }
                // missing, create new
                newIdentMap.append( name, _newUniqueIdent(ns, "index") );
            }
            b.append( "idxIdent", newIdentMap.obj() );

            // add whatever is left
            b.appendElementsUnique( obj );
            obj = b.obj();
        }

        LOG(3) << "recording new metadata: " << obj;
        StatusWith<RecordId> status = _rs->updateRecord( opCtx,
                                                        loc,
                                                        obj.objdata(),
                                                        obj.objsize(),
                                                        false,
                                                        NULL );
        fassert( 28521, status.getStatus() );
        invariant( status.getValue() == loc );
    }

    Status KVCatalog::renameCollection( OperationContext* opCtx,
                                        StringData fromNS,
                                        StringData toNS,
                                        bool stayTemp ) {

        std::unique_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe && opCtx->lockState()) {
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(),
                                             resourceIdCatalogMetadata,
                                             MODE_X));
        }

        RecordId loc;
        BSONObj old = _findEntry( opCtx, fromNS, &loc ).getOwned();
        {
            BSONObjBuilder b;

            b.append( "ns", toNS );

            BSONCollectionCatalogEntry::MetaData md;
            md.parse( old["md"].Obj() );
            md.rename( toNS );
            if ( !stayTemp )
                md.options.temp = false;
            b.append( "md", md.toBSON() );

            b.appendElementsUnique( old );

            BSONObj obj = b.obj();
            StatusWith<RecordId> status = _rs->updateRecord( opCtx,
                                                            loc,
                                                            obj.objdata(),
                                                            obj.objsize(),
                                                            false,
                                                            NULL );
            fassert( 28522, status.getStatus() );
            invariant( status.getValue() == loc );
        }

        boost::lock_guard<boost::mutex> lk( _identsLock );
        const NSToIdentMap::iterator fromIt = _idents.find(fromNS.toString());
        invariant(fromIt != _idents.end());

        opCtx->recoveryUnit()->registerChange(new RemoveIdentChange(this, fromNS, fromIt->second));
        opCtx->recoveryUnit()->registerChange(new AddIdentChange(this, toNS));

        _idents.erase(fromIt);
        _idents[toNS.toString()] = Entry( old["ident"].String(), loc );

        return Status::OK();
    }

    Status KVCatalog::dropCollection( OperationContext* opCtx,
                                      StringData ns ) {
        invariant( opCtx->lockState() == NULL ||
                   opCtx->lockState()->isDbLockedForMode( nsToDatabaseSubstring(ns), MODE_X ) );
        std::unique_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe && opCtx->lockState()) {
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(),
                                             resourceIdCatalogMetadata,
                                             MODE_X));
        }

        boost::lock_guard<boost::mutex> lk( _identsLock );
        const NSToIdentMap::iterator it = _idents.find(ns.toString());
        if (it == _idents.end()) {
            return Status( ErrorCodes::NamespaceNotFound, "collection not found" );
        }

        opCtx->recoveryUnit()->registerChange(new RemoveIdentChange(this, ns, it->second));

        LOG(1) << "deleting metadata for " << ns << " @ " << it->second.storedLoc;
        _rs->deleteRecord( opCtx, it->second.storedLoc );
        _idents.erase(it);

        return Status::OK();
    }

    std::vector<std::string> KVCatalog::getAllIdentsForDB( StringData db ) const {
        std::vector<std::string> v;

        {
            boost::lock_guard<boost::mutex> lk( _identsLock );
            for ( NSToIdentMap::const_iterator it = _idents.begin(); it != _idents.end(); ++it ) {
                NamespaceString ns( it->first );
                if ( ns.db() != db )
                    continue;
                v.push_back( it->second.ident );
            }
        }

        return v;
    }

    std::vector<std::string> KVCatalog::getAllIdents( OperationContext* opCtx ) const {
        std::vector<std::string> v;

        auto cursor = _rs->getCursor(opCtx);
        while (auto record = cursor->next()) {
            BSONObj obj = record->data.releaseToBson();
            v.push_back( obj["ident"].String() );

            BSONElement e = obj["idxIdent"];
            if ( !e.isABSONObj() )
                continue;
            BSONObj idxIdent = e.Obj();

            BSONObjIterator sub( idxIdent );
            while ( sub.more() ) {
                BSONElement e = sub.next();
                v.push_back( e.String() );
            }
        }

        return v;
    }

    bool KVCatalog::isUserDataIdent( StringData ident ) const {
        return
            ident.find( "index-" ) != std::string::npos ||
            ident.find( "index/" ) != std::string::npos ||
            ident.find( "collection-" ) != std::string::npos ||
            ident.find( "collection/" ) != std::string::npos;
    }

}
