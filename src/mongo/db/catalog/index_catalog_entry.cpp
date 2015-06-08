// index_catalog_entry.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog_entry.h"

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/head_manager.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    using std::string;

    class HeadManagerImpl : public HeadManager {
    public:
        HeadManagerImpl(IndexCatalogEntry* ice) : _catalogEntry(ice) { }
        virtual ~HeadManagerImpl() { }

        const RecordId getHead(OperationContext* txn) const {
            return _catalogEntry->head(txn);
        }

        void setHead(OperationContext* txn, const RecordId newHead) {
            _catalogEntry->setHead(txn, newHead);
        }

    private:
        // Not owned here.
        IndexCatalogEntry* _catalogEntry;
    };

    IndexCatalogEntry::IndexCatalogEntry( StringData ns,
                                          CollectionCatalogEntry* collection,
                                          IndexDescriptor* descriptor,
                                          CollectionInfoCache* infoCache )
        : _ns( ns.toString() ),
          _collection( collection ),
          _descriptor( descriptor ),
          _infoCache( infoCache ),
          _accessMethod( NULL ),
          _headManager(new HeadManagerImpl(this)),
          _ordering( Ordering::make( descriptor->keyPattern() ) ),
          _isReady( false ) {

        _descriptor->_cachedEntry = this;
    }

    IndexCatalogEntry::~IndexCatalogEntry() {
        _descriptor->_cachedEntry = NULL; // defensive

        delete _headManager;
        delete _accessMethod;
        delete _descriptor;
    }

    void IndexCatalogEntry::init( OperationContext* txn,
                                  IndexAccessMethod* accessMethod ) {
        verify( _accessMethod == NULL );
        _accessMethod = accessMethod;

        _isReady = _catalogIsReady( txn );
        _head = _catalogHead( txn );
        _isMultikey = _catalogIsMultikey( txn );

        BSONElement filterElement = _descriptor->getInfoElement("partialFilterExpression");
        if ( filterElement.type() ) {
            invariant( filterElement.isABSONObj() );
            BSONObj filter = filterElement.Obj();
            StatusWithMatchExpression res = MatchExpressionParser::parse( filter );
            // this should be checked in create, so can blow up here
            invariantOK( res.getStatus() );
            _filterExpression.reset( res.getValue() );
            LOG(2) << "have filter expression for "
                   << _ns << " " << _descriptor->indexName()
                   << " " << filter;
        }
    }

    const RecordId& IndexCatalogEntry::head( OperationContext* txn ) const {
        DEV invariant( _head == _catalogHead( txn ) );
        return _head;
    }

    bool IndexCatalogEntry::isReady( OperationContext* txn ) const {
        DEV invariant( _isReady == _catalogIsReady( txn ) );
        return _isReady;
    }

    bool IndexCatalogEntry::isMultikey() const {
        return _isMultikey;
    }

    // ---

    void IndexCatalogEntry::setIsReady( bool newIsReady ) {
        _isReady = newIsReady;
    }

    class IndexCatalogEntry::SetHeadChange : public RecoveryUnit::Change {
    public:
        SetHeadChange(IndexCatalogEntry* ice, RecordId oldHead) :_ice(ice), _oldHead(oldHead) {
        }

        virtual void commit() {}
        virtual void rollback() { _ice->_head = _oldHead; }

        IndexCatalogEntry* _ice;
        const RecordId _oldHead;
    };

    void IndexCatalogEntry::setHead( OperationContext* txn, RecordId newHead ) {
        _collection->setIndexHead( txn,
                                   _descriptor->indexName(),
                                   newHead );

        txn->recoveryUnit()->registerChange(new SetHeadChange(this, _head));
        _head = newHead;
    }


    /**
     * RAII class, which associates a new RecoveryUnit with an OperationContext for the purposes
     * of simulating a sub-transaction. Takes ownership of the new recovery unit and frees it at
     * destruction time.
     */
    class RecoveryUnitSwap {
    public:
        RecoveryUnitSwap(OperationContext* txn, RecoveryUnit* newRecoveryUnit)
            : _txn(txn),
              _oldRecoveryUnit(_txn->releaseRecoveryUnit()),
              _oldRecoveryUnitState(_txn->setRecoveryUnit(newRecoveryUnit,
                                                          OperationContext::kNotInUnitOfWork)),
              _newRecoveryUnit(newRecoveryUnit) { }

        ~RecoveryUnitSwap() {
            _txn->releaseRecoveryUnit();
            _txn->setRecoveryUnit(_oldRecoveryUnit, _oldRecoveryUnitState);
        }

    private:
        // Not owned
        OperationContext* const _txn;

        // Owned, but life-time is not controlled
        RecoveryUnit* const _oldRecoveryUnit;
        OperationContext::RecoveryUnitState const _oldRecoveryUnitState;

        // Owned and life-time is controlled
        const boost::scoped_ptr<RecoveryUnit> _newRecoveryUnit;
    };

    void IndexCatalogEntry::setMultikey(OperationContext* txn) {
        if (isMultikey()) {
            return;
        }

        // Only one thread should set the multi-key value per collection, because the metadata for
        // a collection is one large document.
        Lock::ResourceLock collMDLock(txn->lockState(),
                                      ResourceId(RESOURCE_METADATA, _ns),
                                      MODE_X);

        // Check again in case we blocked on the MD lock and another thread beat us to setting the
        // multiKey metadata for this index.
        if (isMultikey()) {
            return;
        }

        // This effectively emulates a sub-transaction off the main transaction, which invoked
        // setMultikey. The reason we need is to avoid artificial WriteConflicts, which happen
        // with snapshot isolation.
        {
            StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
            RecoveryUnitSwap ruSwap(txn, storageEngine->newRecoveryUnit());

            WriteUnitOfWork wuow(txn);

            if (_collection->setIndexIsMultikey(txn, _descriptor->indexName())) {
                if (_infoCache) {
                    LOG(1) << _ns << ": clearing plan cache - index "
                           << _descriptor->keyPattern() << " set to multi key.";
                    _infoCache->clearQueryCache();
                }
            }

            wuow.commit();
        }

        _isMultikey = true;
    }

    // ----

    bool IndexCatalogEntry::_catalogIsReady( OperationContext* txn ) const {
        return _collection->isIndexReady( txn, _descriptor->indexName() );
    }

    RecordId IndexCatalogEntry::_catalogHead( OperationContext* txn ) const {
        return _collection->getIndexHead( txn, _descriptor->indexName() );
    }

    bool IndexCatalogEntry::_catalogIsMultikey( OperationContext* txn ) const {
        return _collection->isIndexMultikey( txn, _descriptor->indexName() );
    }

    // ------------------

    const IndexCatalogEntry* IndexCatalogEntryContainer::find( const IndexDescriptor* desc ) const {
        if ( desc->_cachedEntry )
            return desc->_cachedEntry;

        for ( const_iterator i = begin(); i != end(); ++i ) {
            const IndexCatalogEntry* e = *i;
            if ( e->descriptor() == desc )
                    return e;
        }
        return NULL;
    }

    IndexCatalogEntry* IndexCatalogEntryContainer::find( const IndexDescriptor* desc ) {
        if ( desc->_cachedEntry )
            return desc->_cachedEntry;

        for ( iterator i = begin(); i != end(); ++i ) {
            IndexCatalogEntry* e = *i;
            if ( e->descriptor() == desc )
                return e;
        }
        return NULL;
    }

    IndexCatalogEntry* IndexCatalogEntryContainer::find( const string& name ) {
        for ( iterator i = begin(); i != end(); ++i ) {
            IndexCatalogEntry* e = *i;
            if ( e->descriptor()->indexName() == name )
                return e;
        }
        return NULL;
    }

    IndexCatalogEntry* IndexCatalogEntryContainer::release( const IndexDescriptor* desc ) {
        for ( std::vector<IndexCatalogEntry*>::iterator i = _entries.mutableVector().begin();
              i != _entries.mutableVector().end();
              ++i ) {
            IndexCatalogEntry* e = *i;
            if ( e->descriptor() != desc )
                continue;
            _entries.mutableVector().erase( i );
            return e;
        }
        return NULL;
    }

}  // namespace mongo
