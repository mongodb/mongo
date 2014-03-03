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

#include "mongo/db/index/btree_access_method.h"

#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/curop.h"
#include "mongo/db/extsort.h"
#include "mongo/db/index/btree_index_cursor.h"
#include "mongo/db/index/btree_interface.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/sort_phase_one.h"
#include "mongo/db/structure/btree/btreebuilder.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

    BtreeBasedAccessMethod::BtreeBasedAccessMethod(IndexCatalogEntry* btreeState)
        : _btreeState(btreeState), _descriptor(btreeState->descriptor()) {

        verify(0 == _descriptor->version() || 1 == _descriptor->version());
        _interface = BtreeInterface::interfaces[_descriptor->version()];
    }

    // Find the keys for obj, put them in the tree pointing to loc
    Status BtreeBasedAccessMethod::insert(const BSONObj& obj, const DiskLoc& loc,
            const InsertDeleteOptions& options, int64_t* numInserted) {

        *numInserted = 0;

        BSONObjSet keys;
        // Delegate to the subclass.
        getKeys(obj, &keys);

        Status ret = Status::OK();

        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            try {
                _interface->bt_insert(_btreeState,
                                      _btreeState->head(),
                                      loc,
                                      *i,
                                      options.dupsAllowed,
                                      true);
                ++*numInserted;
            } catch (AssertionException& e) {
                if (10287 == e.getCode() && !_btreeState->isReady()) {
                    // This is the duplicate key exception.  We ignore it for some reason in BG
                    // indexing.
                    DEV log() << "info: key already in index during bg indexing (ok)\n";
                } else if (!options.dupsAllowed) {
                    // Assuming it's a duplicate key exception.  Clean up any inserted keys.
                    for (BSONObjSet::const_iterator j = keys.begin(); j != i; ++j) {
                        removeOneKey(*j, loc);
                    }
                    *numInserted = 0;
                    return Status(ErrorCodes::DuplicateKey, e.what(), e.getCode());
                } else {
                    problem() << " caught assertion addKeysToIndex "
                              << _descriptor->indexNamespace()
                              << obj["_id"] << endl;
                    ret = Status(ErrorCodes::InternalError, e.what(), e.getCode());
                }
            }
        }

        if (*numInserted > 1) {
            _btreeState->setMultikey();
        }

        return ret;
    }

    bool BtreeBasedAccessMethod::removeOneKey(const BSONObj& key, const DiskLoc& loc) {
        bool ret = false;

        try {
            ret = _interface->unindex(_btreeState,
                                      _btreeState->head(),
                                      key,
                                      loc);
        } catch (AssertionException& e) {
            problem() << "Assertion failure: _unindex failed "
                << _descriptor->indexNamespace() << endl;
            out() << "Assertion failure: _unindex failed: " << e.what() << '\n';
            out() << "  obj:" << loc.obj().toString() << '\n';
            out() << "  key:" << key.toString() << '\n';
            out() << "  dl:" << loc.toString() << endl;
            logContext();
        }

        return ret;
    }

    Status BtreeBasedAccessMethod::newCursor(IndexCursor **out) const {
        *out = new BtreeIndexCursor(_btreeState, _interface);
        return Status::OK();
    }

    // Remove the provided doc from the index.
    Status BtreeBasedAccessMethod::remove(const BSONObj &obj, const DiskLoc& loc,
        const InsertDeleteOptions &options, int64_t* numDeleted) {

        BSONObjSet keys;
        getKeys(obj, &keys);
        *numDeleted = 0;

        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            bool thisKeyOK = removeOneKey(*i, loc);

            if (thisKeyOK) {
                ++*numDeleted;
            } else if (options.logIfError) {
                log() << "unindex failed (key too big?) " << _descriptor->indexNamespace()
                      << " key: " << *i << " " << loc.obj()["_id"] << endl;
            }
        }

        return Status::OK();
    }

    // Return keys in l that are not in r.
    // Lifted basically verbatim from elsewhere.
    static void setDifference(const BSONObjSet &l, const BSONObjSet &r, vector<BSONObj*> *diff) {
        // l and r must use the same ordering spec.
        verify(l.key_comp().order() == r.key_comp().order());
        BSONObjSet::const_iterator i = l.begin();
        BSONObjSet::const_iterator j = r.begin();
        while ( 1 ) {
            if ( i == l.end() )
                break;
            while ( j != r.end() && j->woCompare( *i ) < 0 )
                j++;
            if ( j == r.end() || i->woCompare(*j) != 0  ) {
                const BSONObj *jo = &*i;
                diff->push_back( (BSONObj *) jo );
            }
            i++;
        }
    }

    Status BtreeBasedAccessMethod::initializeAsEmpty() {
        if ( !_btreeState->head().isNull() )
            return Status( ErrorCodes::InternalError, "index already initialized" );

        DiskLoc newHead;
        if ( 0 == _descriptor->version() ) {
            newHead = BtreeBucket<V0>::addBucket( _btreeState );
        }
        else if ( 1 == _descriptor->version() ) {
            newHead = BtreeBucket<V1>::addBucket( _btreeState );
        }
        else {
            return Status( ErrorCodes::InternalError, "invalid index number" );
        }
        _btreeState->setHead( newHead );

        return Status::OK();
    }

    Status BtreeBasedAccessMethod::touch(const BSONObj& obj) {
        BSONObjSet keys;
        getKeys(obj, &keys);

        for (BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i) {
            int unusedPos;
            bool unusedFound;
            DiskLoc unusedDiskLoc;
            _interface->locate(_btreeState,
                               _btreeState->head(),
                               *i,
                               unusedPos,
                               unusedFound,
                               unusedDiskLoc,
                               1);
        }

        return Status::OK();
    }

    DiskLoc BtreeBasedAccessMethod::findSingle( const BSONObj& key ) const {
        DiskLoc head = _btreeState->head();
        Record* record = _btreeState->recordStore()->recordFor( head );

        if ( 0 == _descriptor->version() ) {
            return BtreeBucket<V0>::asVersion( record )->findSingle( _btreeState,
                                                                     _btreeState->head(),
                                                                     key );
        }
        if ( 1 == _descriptor->version() ) {
            return BtreeBucket<V1>::asVersion( record )->findSingle( _btreeState,
                                                                     _btreeState->head(),
                                                                     key );
        }
        verify( 0 );
    }


    Status BtreeBasedAccessMethod::validate(int64_t* numKeys) {
        *numKeys = _interface->fullValidate(_btreeState,
                                            _btreeState->head(),
                                            _descriptor->keyPattern());
        return Status::OK();
    }

    Status BtreeBasedAccessMethod::validateUpdate(
        const BSONObj &from, const BSONObj &to, const DiskLoc &record,
        const InsertDeleteOptions &options, UpdateTicket* status) {

        BtreeBasedPrivateUpdateData *data = new BtreeBasedPrivateUpdateData();
        status->_indexSpecificUpdateData.reset(data);

        getKeys(from, &data->oldKeys);
        getKeys(to, &data->newKeys);
        data->loc = record;
        data->dupsAllowed = options.dupsAllowed;

        setDifference(data->oldKeys, data->newKeys, &data->removed);
        setDifference(data->newKeys, data->oldKeys, &data->added);

        bool checkForDups = !data->added.empty()
            && (KeyPattern::isIdKeyPattern(_descriptor->keyPattern()) || _descriptor->unique())
            && !options.dupsAllowed;

        if (checkForDups) {
            for (vector<BSONObj*>::iterator i = data->added.begin(); i != data->added.end(); i++) {
                if (_interface->wouldCreateDup(_btreeState,
                                               _btreeState->head(),
                                               **i, record)) {
                    status->_isValid = false;
                    return Status(ErrorCodes::DuplicateKey,
                                  _interface->dupKeyError(_btreeState,
                                                          _btreeState->head(),
                                                          **i));
                }
            }
        }

        status->_isValid = true;

        return Status::OK();
    }

    Status BtreeBasedAccessMethod::update(const UpdateTicket& ticket, int64_t* numUpdated) {
        if (!ticket._isValid) {
            return Status(ErrorCodes::InternalError, "Invalid updateticket in update");
        }

        BtreeBasedPrivateUpdateData* data =
            static_cast<BtreeBasedPrivateUpdateData*>(ticket._indexSpecificUpdateData.get());

        if (data->oldKeys.size() + data->added.size() - data->removed.size() > 1) {
            _btreeState->setMultikey();
        }

        for (size_t i = 0; i < data->added.size(); ++i) {
            _interface->bt_insert(_btreeState,
                                  _btreeState->head(),
                                  data->loc,
                                  *data->added[i],
                                  data->dupsAllowed,
                                  true);
        }

        for (size_t i = 0; i < data->removed.size(); ++i) {
            _interface->unindex(_btreeState,
                                _btreeState->head(),
                                *data->removed[i],
                                data->loc);
        }

        *numUpdated = data->added.size();

        return Status::OK();
    }

    // -------

    class BtreeBulk : public IndexAccessMethod {
    public:
        BtreeBulk( BtreeBasedAccessMethod* real ) {
            _real = real;
        }

        ~BtreeBulk() {}

        virtual Status insert(const BSONObj& obj,
                              const DiskLoc& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numInserted) {
            BSONObjSet keys;
            _real->getKeys(obj, &keys);
            _phase1.addKeys(keys, loc, false);
            if ( numInserted )
                *numInserted += keys.size();
            return Status::OK();
        }

        virtual Status remove(const BSONObj& obj,
                              const DiskLoc& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numDeleted) {
            return _notAllowed();
        }

        virtual Status validateUpdate(const BSONObj& from,
                                      const BSONObj& to,
                                      const DiskLoc& loc,
                                      const InsertDeleteOptions& options,
                                      UpdateTicket* ticket) {
            return _notAllowed();
        }

        virtual Status update(const UpdateTicket& ticket, int64_t* numUpdated) {
            return _notAllowed();
        }

        virtual Status newCursor(IndexCursor **out) const {
            return _notAllowed();
        }

        virtual Status initializeAsEmpty() {
            return _notAllowed();
        }

        virtual IndexAccessMethod* initiateBulk() {
            return this;
        }

        virtual Status commitBulk( IndexAccessMethod* bulk,
                                   bool mayInterrupt,
                                   std::set<DiskLoc>* dups ) {
            verify( this == bulk );
            return Status::OK();
        }

        virtual Status touch(const BSONObj& obj) {
            return _notAllowed();
        }

        virtual Status validate(int64_t* numKeys) {
            return _notAllowed();
        }

        // -------

        template< class V >
        void commit( set<DiskLoc>* dupsToDrop,
                     CurOp* op,
                     bool mayInterrupt ) {

            Timer timer;

            IndexCatalogEntry* entry = _real->_btreeState;

            bool dupsAllowed = !entry->descriptor()->unique() ||
                ignoreUniqueIndex(entry->descriptor());
            bool dropDups = entry->descriptor()->dropDups() || inDBRepair;

            BtreeBuilder<V> btBuilder(dupsAllowed, entry);

            BSONObj keyLast;
            scoped_ptr<BSONObjExternalSorter::Iterator> i( _phase1.sorter->iterator() );

            // verifies that pm and op refer to the same ProgressMeter
            ProgressMeter& pm = op->setMessage("Index Bulk Build: (2/3) btree bottom up",
                                               "Index: (2/3) BTree Bottom Up Progress",
                                               _phase1.nkeys,
                                               10);

            while( i->more() ) {
                RARELY if ( mayInterrupt ) killCurrentOp.checkForInterrupt();
                ExternalSortDatum d = i->next();

                try {
                    if ( !dupsAllowed && dropDups ) {
                        LastError::Disabled led( lastError.get() );
                        btBuilder.addKey(d.first, d.second);
                    }
                    else {
                        btBuilder.addKey(d.first, d.second);
                    }
                }
                catch( AssertionException& e ) {
                    if ( dupsAllowed ) {
                        // unknown exception??
                        throw;
                    }

                    if (ErrorCodes::isInterruption(
                            DBException::convertExceptionCode(e.getCode()))) {
                        killCurrentOp.checkForInterrupt();
                    }

                    if ( ! dropDups )
                        throw;

                    /* we could queue these on disk, but normally there are very few dups,
                     * so instead we keep in ram and have a limit.
                    */
                    if ( dupsToDrop ) {
                        dupsToDrop->insert(d.second);
                        uassert( 10092,
                                 "too may dups on index build with dropDups=true",
                                 dupsToDrop->size() < 1000000 );
                    }
                }
                pm.hit();
            }
            pm.finished();
            op->setMessage("Index Bulk Build: (3/3) btree-middle",
                           "Index: (3/3) BTree Middle Progress");
            LOG(timer.seconds() > 10 ? 0 : 1 ) << "\t done building bottom layer, going to commit";
            btBuilder.commit( mayInterrupt );
            if ( btBuilder.getn() != _phase1.nkeys && ! dropDups ) {
                warning() << "not all entries were added to the index, probably some "
                          << "keys were too large" << endl;
            }
        }

        // -------

        Status _notAllowed() const {
            return Status( ErrorCodes::InternalError, "cannot use bulk for this yet" );
        }

        BtreeBasedAccessMethod* _real; // now owned here
        SortPhaseOne _phase1;
    };

    int oldCompare(const BSONObj& l,const BSONObj& r, const Ordering &o); // key.cpp

    class BtreeExternalSortComparisonV0 : public ExternalSortComparison {
    public:
        BtreeExternalSortComparisonV0(const BSONObj& ordering)
            : _ordering(Ordering::make(ordering)){
        }

        virtual ~BtreeExternalSortComparisonV0() { }

        virtual int compare(const ExternalSortDatum& l, const ExternalSortDatum& r) const {
            int x = oldCompare(l.first, r.first, _ordering);
            if (x) { return x; }
            return l.second.compare(r.second);
        }
    private:
        const Ordering _ordering;
    };

    class BtreeExternalSortComparisonV1 : public ExternalSortComparison {
    public:
        BtreeExternalSortComparisonV1(const BSONObj& ordering)
            : _ordering(Ordering::make(ordering)) {
        }

        virtual ~BtreeExternalSortComparisonV1() { }

        virtual int compare(const ExternalSortDatum& l, const ExternalSortDatum& r) const {
            int x = l.first.woCompare(r.first, _ordering, /*considerfieldname*/false);
            if (x) { return x; }
            return l.second.compare(r.second);
        }
    private:
        const Ordering _ordering;
    };

    ExternalSortComparison* BtreeBasedAccessMethod::getComparison(int version,
                                                                  const BSONObj& keyPattern) {

        if ( 0 == version ) {
            return new BtreeExternalSortComparisonV0( keyPattern );
        }
        else if ( 1 == version ) {
            return new BtreeExternalSortComparisonV1( keyPattern );
        }
        verify( 0 );
        return NULL;
    }

    IndexAccessMethod* BtreeBasedAccessMethod::initiateBulk() {

        if ( _interface->nKeys( _btreeState,
                                _btreeState->head() ) > 0 )
            return NULL;

        auto_ptr<BtreeBulk> bulk( new BtreeBulk( this ) );
        bulk->_phase1.sortCmp.reset( getComparison( _descriptor->version(),
                                                    _descriptor->keyPattern() ) );

        bulk->_phase1.sorter.reset( new BSONObjExternalSorter(bulk->_phase1.sortCmp.get()) );
        bulk->_phase1.sorter->hintNumObjects( _btreeState->collection()->numRecords() );

        return bulk.release();
    }

    Status BtreeBasedAccessMethod::commitBulk( IndexAccessMethod* bulkRaw,
                                               bool mayInterrupt,
                                               set<DiskLoc>* dupsToDrop ) {

        if ( _interface->nKeys( _btreeState,
                                _btreeState->head() ) > 0 ) {
            return Status( ErrorCodes::InternalError, "trying to commit, but has data already" );
        }

        {
            DiskLoc oldHead = _btreeState->head();
            _btreeState->setHead( DiskLoc() );
            _btreeState->recordStore()->deleteRecord( oldHead );
        }

        string ns = _btreeState->collection()->ns().ns();

        BtreeBulk* bulk = static_cast<BtreeBulk*>( bulkRaw );
        if ( bulk->_phase1.multi )
            _btreeState->setMultikey();

        bulk->_phase1.sorter->sort( false );

        if ( _descriptor->version() == 0 )
            bulk->commit<V0>( dupsToDrop, cc().curop(), mayInterrupt );
        else if ( _descriptor->version() == 1 )
            bulk->commit<V1>( dupsToDrop, cc().curop(), mayInterrupt );
        else
            return Status( ErrorCodes::InternalError, "bad btree version" );

        return Status::OK();
    }


}  // namespace mongo
