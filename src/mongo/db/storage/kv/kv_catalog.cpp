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
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/platform/random.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
    // This is never used with KVEngines that support doc-level locking so this should never
    // conflict with anything else.
    // This must be locked *before* _identLock.
    const ResourceId catalogRID(RESOURCE_DOCUMENT, StringData("KVCatalog"));
}

    class KVCatalog::AddIdentChange : public RecoveryUnit::Change {
    public:
        AddIdentChange(KVCatalog* catalog, const StringData& ident)
            :_catalog(catalog), _ident(ident.toString())
        {}

        virtual void commit() {}
        virtual void rollback() {
            boost::mutex::scoped_lock lk(_catalog->_identsLock);
            _catalog->_idents.erase(_ident);
        }

        KVCatalog* const _catalog;
        const std::string _ident;
    };

    class KVCatalog::RemoveIdentChange : public RecoveryUnit::Change {
    public:
        RemoveIdentChange(KVCatalog* catalog, const StringData& ident, const Entry& entry)
            :_catalog(catalog), _ident(ident.toString()), _entry(entry)
        {}

        virtual void commit() {}
        virtual void rollback() {
            boost::mutex::scoped_lock lk(_catalog->_identsLock);
            _catalog->_idents[_ident] = _entry;
        }

        KVCatalog* const _catalog;
        const std::string _ident;
        const Entry _entry;
    };

    KVCatalog::KVCatalog( RecordStore* rs, bool isRsThreadSafe )
        : _rs( rs ), _isRsThreadSafe(isRsThreadSafe) {
        boost::scoped_ptr<SecureRandom> r( SecureRandom::create() );
        _rand = r->nextInt64();
    }

    KVCatalog::~KVCatalog() {
        _rs = NULL;
    }

    void KVCatalog::init( OperationContext* opCtx ) {
        // No locking needed since called single threaded.
        scoped_ptr<RecordIterator> it( _rs->getIterator( opCtx ) );
        while ( !it->isEOF()  ) {
            DiskLoc loc = it->getNext();
            RecordData data = it->dataFor( loc );
            BSONObj obj( data.data() );

            // No locking needed since can only be called from one thread.
            // No rollback since this is just loading already committed data.
            string ns = obj["ns"].String();
            string ident = obj["ident"].String();
            _idents[ns] = Entry( ident, loc );
        }
    }

    void KVCatalog::getAllCollections( std::vector<std::string>* out ) const {
        boost::mutex::scoped_lock lk( _identsLock );
        for ( NSToIdentMap::const_iterator it = _idents.begin(); it != _idents.end(); ++it ) {
            out->push_back( it->first );
        }
    }

    Status KVCatalog::newCollection( OperationContext* opCtx,
                                     const StringData& ns,
                                     const CollectionOptions& options ) {
        boost::scoped_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe)
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(), catalogRID, MODE_X));

        std::stringstream ss;
        ss << "collection-" << _rand << "-" << _next.fetchAndAdd( 1 );
        string ident = ss.str();

        boost::mutex::scoped_lock lk( _identsLock );
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

        StatusWith<DiskLoc> res = _rs->insertRecord( opCtx, obj.objdata(), obj.objsize(), false );
        if ( !res.isOK() )
            return res.getStatus();

        old = Entry( ident, res.getValue() );
        LOG(1) << "stored meta data for " << ns << " @ " << res.getValue();
        return Status::OK();
    }

    std::string KVCatalog::getCollectionIdent( const StringData& ns ) const {
        boost::mutex::scoped_lock lk( _identsLock );
        NSToIdentMap::const_iterator it = _idents.find( ns.toString() );
        invariant( it != _idents.end() );
        return it->second.ident;
    }

    std::string KVCatalog::getIndexIdent( OperationContext* opCtx,
                                          const StringData& ns,
                                          const StringData& idxName ) const {
        BSONObj obj = _findEntry( opCtx, ns );
        BSONObj idxIdent = obj["idxIdent"].Obj();
        return idxIdent[idxName].String();
    }

    BSONObj KVCatalog::_findEntry( OperationContext* opCtx,
                                   const StringData& ns,
                                   DiskLoc* out ) const {

        boost::scoped_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe)
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(), catalogRID, MODE_S));

        DiskLoc dl;
        {
            boost::mutex::scoped_lock lk( _identsLock );
            NSToIdentMap::const_iterator it = _idents.find( ns.toString() );
            invariant( it != _idents.end() );
            dl = it->second.storedLoc;
        }

        LOG(1) << "looking up metadata for: " << ns << " @ " << dl;
        RecordData data = _rs->dataFor( opCtx, dl );

        if (out)
            *out = dl;

        return data.toBson().getOwned();
    }

    const BSONCollectionCatalogEntry::MetaData KVCatalog::getMetaData( OperationContext* opCtx,
                                                                       const StringData& ns ) {
        BSONObj obj = _findEntry( opCtx, ns );
        LOG(1) << " got: " << obj;
        BSONCollectionCatalogEntry::MetaData md;
        if ( obj["md"].isABSONObj() )
            md.parse( obj["md"].Obj() );
        return md;
    }

    void KVCatalog::putMetaData( OperationContext* opCtx,
                                 const StringData& ns,
                                 BSONCollectionCatalogEntry::MetaData& md ) {
        boost::scoped_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe)
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(), catalogRID, MODE_X));

        DiskLoc loc;
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
                std::stringstream ss;
                ss << getCollectionIdent( ns ) << '$' << name
                   << '-' << _rand << '-' << _next.fetchAndAdd( 1 );
                newIdentMap.append( name, ss.str() );
            }
            b.append( "idxIdent", newIdentMap.obj() );

            // add whatever is left
            b.appendElementsUnique( obj );
            obj = b.obj();
        }

        StatusWith<DiskLoc> status = _rs->updateRecord( opCtx,
                                                        loc,
                                                        obj.objdata(),
                                                        obj.objsize(),
                                                        false,
                                                        NULL );
        fassert( 28521, status.getStatus() );
        invariant( status.getValue() == loc );
    }

    Status KVCatalog::renameCollection( OperationContext* opCtx,
                                        const StringData& fromNS,
                                        const StringData& toNS,
                                        bool stayTemp ) {
        boost::scoped_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe)
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(), catalogRID, MODE_X));

        DiskLoc loc;
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
            StatusWith<DiskLoc> status = _rs->updateRecord( opCtx,
                                                            loc,
                                                            obj.objdata(),
                                                            obj.objsize(),
                                                            false,
                                                            NULL );
            fassert( 28522, status.getStatus() );
            invariant( status.getValue() == loc );
        }

        boost::mutex::scoped_lock lk( _identsLock );
        const NSToIdentMap::iterator fromIt = _idents.find(fromNS.toString());
        invariant(fromIt != _idents.end());

        opCtx->recoveryUnit()->registerChange(new RemoveIdentChange(this, fromNS, fromIt->second));
        opCtx->recoveryUnit()->registerChange(new AddIdentChange(this, toNS));

        _idents.erase(fromIt);
        _idents[toNS.toString()] = Entry( old["ident"].String(), loc );

        return Status::OK();
    }

    Status KVCatalog::dropCollection( OperationContext* opCtx,
                                      const StringData& ns ) {
        boost::scoped_ptr<Lock::ResourceLock> rLk;
        if (!_isRsThreadSafe)
            rLk.reset(new Lock::ResourceLock(opCtx->lockState(), catalogRID, MODE_X));

        boost::mutex::scoped_lock lk( _identsLock );
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


}
