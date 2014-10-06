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

#include "mongo/db/storage/kv/kv_catalog.h"

#include <stdlib.h>

#include "mongo/db/storage/record_store.h"
#include "mongo/platform/random.h"
#include "mongo/util/log.h"

namespace mongo {

    KVCatalog::KVCatalog( RecordStore* rs )
        : _rs( rs ) {
        boost::scoped_ptr<SecureRandom> r( SecureRandom::create() );
        _rand = r->nextInt64();
    }

    KVCatalog::~KVCatalog() {
        _rs = NULL;
    }

    void KVCatalog::init( OperationContext* opCtx ) {
        scoped_ptr<RecordIterator> it( _rs->getIterator( opCtx ) );
        while ( !it->isEOF()  ) {
            DiskLoc loc = it->getNext();
            RecordData data = it->dataFor( loc );
            BSONObj obj( data.data() );

            // no locking needed since can only be one
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
        std::stringstream ss;
        ss << ns << "-" << _rand << "-" << _next.fetchAndAdd( 1 );
        string ident = ss.str();

        boost::mutex::scoped_lock lk( _identsLock );
        Entry& old = _idents[ns.toString()];
        if ( !old.ident.empty() ) {
            return Status( ErrorCodes::NamespaceExists, "collection already exists" );
        }

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
        DiskLoc loc;
        BSONObj obj = _findEntry( opCtx, ns, &loc );
        BSONObj idxIdent = obj["idxIdent"].Obj();
        return idxIdent[idxName].String();
    }

    BSONObj KVCatalog::_findEntry( OperationContext* opCtx,
                                   const StringData& ns,
                                   DiskLoc* out ) const {
        {
            boost::mutex::scoped_lock lk( _identsLock );
            NSToIdentMap::const_iterator it = _idents.find( ns.toString() );
            invariant( it != _idents.end() );
            *out = it->second.storedLoc;
        }
        LOG(1) << "looking up metadata for: " << ns << " @ " << *out;
        RecordData data = _rs->dataFor( opCtx, *out );
        return data.toBson().getOwned();
    }

    const BSONCollectionCatalogEntry::MetaData KVCatalog::getMetaData( OperationContext* opCtx,
                                                                       const StringData& ns ) {
        DiskLoc loc;
        BSONObj obj = _findEntry( opCtx, ns, &loc );
        LOG(1) << " got: " << obj;
        BSONCollectionCatalogEntry::MetaData md;
        if ( obj["md"].isABSONObj() )
            md.parse( obj["md"].Obj() );
        return md;
    }

    void KVCatalog::putMetaData( OperationContext* opCtx,
                                 const StringData& ns,
                                 BSONCollectionCatalogEntry::MetaData& md ) {
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
        _idents.erase( fromNS.toString() );
        _idents[toNS.toString()] = Entry( old["ident"].String(), loc );

        return Status::OK();
    }

    Status KVCatalog::dropCollection( OperationContext* opCtx,
                                      const StringData& ns ) {
        boost::mutex::scoped_lock lk( _identsLock );
        Entry old = _idents[ns.toString()];
        if ( old.ident.empty() ) {
            return Status( ErrorCodes::NamespaceNotFound, "collection not found" );
        }

        LOG(1) << "deleting metadat for " << ns << " @ " << old.storedLoc;
        _rs->deleteRecord( opCtx, old.storedLoc );
        _idents.erase( ns.toString() );

        return Status::OK();
    }


}
