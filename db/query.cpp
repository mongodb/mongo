// query.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
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
 */

#include "pch.h"
#include "query.h"
#include "pdfile.h"
#include "jsobjmanipulator.h"
#include "../bson/util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "json.h"
#include "repl.h"
#include "replpair.h"
#include "scanandorder.h"
#include "security.h"
#include "curop.h"
#include "commands.h"
#include "queryoptimizer.h"
#include "lasterror.h"
#include "../s/d_logic.h"
#include "repl_block.h"

namespace mongo {

    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    const int MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

    //ns->query->DiskLoc
//    LRUishMap<BSONObj,DiskLoc,5> lrutest(123);

    extern bool useCursors;
    extern bool useHints;

    // Just try to identify best plan.
    class DeleteOp : public MultiCursor::CursorOp {
    public:
        DeleteOp( bool justOne, int& bestCount ) :
            justOne_( justOne ),
            count_(),
            bestCount_( bestCount ),
            _nscanned() {
        }
        virtual void _init() {
            c_ = qp().newCursor();
        }
        virtual bool prepareToYield() {
            if ( ! _cc ) {
                _cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , c_ , qp().ns() ) );
            }
            return _cc->prepareToYield( _yieldData );
        }        
        virtual void recoverFromYield() {
            if ( !ClientCursor::recoverFromYield( _yieldData ) ) {
                _cc.reset();
                c_.reset();
                massert( 13340, "cursor dropped during delete", false );
            }
        }
        virtual long long nscanned() {
            assert( c_.get() );
            return c_->nscanned();
        }
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }
            
            DiskLoc rloc = c_->currLoc();
            
            if ( matcher()->matches(c_->currKey(), rloc ) ) {
                if ( !c_->getsetdup(rloc) )
                    ++count_;
            }

            c_->advance();
            _nscanned = c_->nscanned();
            if ( count_ > bestCount_ )
                bestCount_ = count_;
            
            if ( count_ > 0 ) {
                if ( justOne_ )
                    setComplete();
                else if ( _nscanned >= 100 && count_ == bestCount_ )
                    setComplete();
            }
        }
        virtual bool mayRecordPlan() const { return !justOne_; }
        virtual QueryOp *_createChild() const {
            bestCount_ = 0; // should be safe to reset this in contexts where createChild() is called
            return new DeleteOp( justOne_, bestCount_ );
        }
        virtual shared_ptr<Cursor> newCursor() const { return qp().newCursor(); }
    private:
        bool justOne_;
        int count_;
        int &bestCount_;
        long long _nscanned;
        shared_ptr<Cursor> c_;
        ClientCursor::CleanupPointer _cc;
        ClientCursor::YieldData _yieldData;
    };
    
    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
       god:     allow access to system namespaces, and don't yield
    */
    long long deleteObjects(const char *ns, BSONObj pattern, bool justOneOrig, bool logop, bool god, RemoveSaver * rs ) {
        if( !god ) {
            if ( strstr(ns, ".system.") ) {
                /* note a delete from system.indexes would corrupt the db 
                if done here, as there are pointers into those objects in 
                NamespaceDetails.
                */
                uassert(12050, "cannot delete from system namespace", legalClientSystemNS( ns , true ) );
            }
            if ( strchr( ns , '$' ) ){
                log() << "cannot delete from collection with reserved $ in name: " << ns << endl;
                uassert( 10100 ,  "cannot delete from collection with reserved $ in name", strchr(ns, '$') == 0 );
            }
        }

        NamespaceDetails *d = nsdetails( ns );
        if ( ! d )
            return 0;
        uassert( 10101 ,  "can't remove from a capped collection" , ! d->capped );

        long long nDeleted = 0;

        int best = 0;
        shared_ptr< MultiCursor::CursorOp > opPtr( new DeleteOp( justOneOrig, best ) );
        shared_ptr< MultiCursor > creal( new MultiCursor( ns, pattern, BSONObj(), opPtr, true ) );
        
        if( !creal->ok() )
            return nDeleted;
            
        shared_ptr< Cursor > cPtr = creal;
        auto_ptr<ClientCursor> cc( new ClientCursor( QueryOption_NoCursorTimeout, cPtr, ns) );
        cc->setDoingDeletes( true );
            
        CursorId id = cc->cursorid;
            
        bool justOne = justOneOrig;
        bool canYield = !god && !creal->matcher()->docMatcher().atomic();
        do {
            if ( canYield && ! cc->yieldSometimes() ){
                cc.release(); // has already been deleted elsewhere
                // TODO should we assert or something?
                break;
            }
            if ( !cc->c->ok() ) {
                break; // if we yielded, could have hit the end
            }
                
            // this way we can avoid calling updateLocation() every time (expensive)
            // as well as some other nuances handled
            cc->setDoingDeletes( true );
                
            DiskLoc rloc = cc->c->currLoc();
            BSONObj key = cc->c->currKey();

            // NOTE Calling advance() may change the matcher, so it's important 
            // to try to match first.
            bool match = creal->matcher()->matches( key , rloc );
            
            if ( ! cc->c->advance() )
                justOne = true;
                
            if ( ! match )
                continue;
                            
            assert( !cc->c->getsetdup(rloc) ); // can't be a dup, we deleted it!
                
            if ( !justOne ) {
                /* NOTE: this is SLOW.  this is not good, noteLocation() was designed to be called across getMore
                    blocks.  here we might call millions of times which would be bad.
                    */
                cc->c->noteLocation();
            }
                
            if ( logop ) {
                BSONElement e;
                if( BSONObj( rloc.rec() ).getObjectID( e ) ) {
                    BSONObjBuilder b;
                    b.append( e );
                    bool replJustOne = true;
                    logOp( "d", ns, b.done(), 0, &replJustOne );
                } else {
                    problem() << "deleted object without id, not logging" << endl;
                }
            }

            if ( rs )
                rs->goingToDelete( rloc.obj() /*cc->c->current()*/ );

            theDataFileMgr.deleteRecord(ns, rloc.rec(), rloc);
            nDeleted++;
            if ( justOne ) {
                break;
            }
            cc->c->checkLocation();
                
        } while ( cc->c->ok() );

        if ( cc.get() && ClientCursor::find( id , false ) == 0 ){
            cc.release();
        }
              
        return nDeleted;
    }

    int otherTraceLevel = 0;

    int initialExtentSize(int len);

    bool runCommands(const char *ns, BSONObj& jsobj, CurOp& curop, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        try {
            return _runCommands(ns, jsobj, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch ( AssertionException& e ) {
            e.getInfo().append( anObjBuilder , "assertion" , "assertionCode" );
        }
        curop.debug().str << " assertion ";
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.appendBuf((void*) x.objdata(), x.objsize());
        return true;
    }

    int nCaught = 0;

    void killCursors(int n, long long *ids) {
        int k = 0;
        for ( int i = 0; i < n; i++ ) {
            if ( ClientCursor::erase(ids[i]) )
                k++;
        }
        if ( logLevel > 0 || k != n ){
            log( k == n ) << "killcursors: found " << k << " of " << n << endl;
        }
    }

    BSONObj id_obj = fromjson("{\"_id\":1}");
    BSONObj empty_obj = fromjson("{}");


    //int dump = 0;

    /* empty result for error conditions */
    QueryResult* emptyMoreResult(long long cursorid) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        QueryResult *qr = (QueryResult *) b.buf();
        qr->cursorId = 0; // 0 indicates no more data to retrieve.
        qr->startingFrom = 0;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->nReturned = 0;
        b.decouple();
        return qr;
    }

    QueryResult* processGetMore(const char *ns, int ntoreturn, long long cursorid , CurOp& curop, int pass, bool& exhaust ) {
//        log() << "TEMP GETMORE " << ns << ' ' << cursorid << ' ' << pass << endl;
        exhaust = false;
        ClientCursor::Pointer p(cursorid);
        ClientCursor *cc = p.c();
        
        int bufSize = 512;
        if ( cc ){
            bufSize += sizeof( QueryResult );
            bufSize += ( ntoreturn ? 4 : 1 ) * 1024 * 1024;
        }

        BufBuilder b( bufSize );

        b.skip(sizeof(QueryResult));
        
        int resultFlags = ResultFlag_AwaitCapable;
        int start = 0;
        int n = 0;

        if ( !cc ) {
            log() << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = ResultFlag_CursorNotFound;
        }
        else {
            if ( pass == 0 )
                cc->updateSlaveLocation( curop );

            int queryOptions = cc->_queryOptions;

            if( pass == 0 ) {
                StringBuilder& ss = curop.debug().str;
                ss << " getMore: " << cc->query.toString() << " ";
            }
            
            start = cc->pos;
            Cursor *c = cc->c.get();
            c->checkLocation();
            DiskLoc last;

            while ( 1 ) {
                if ( !c->ok() ) {
                    if ( c->tailable() ) {
                        /* when a tailable cursor hits "EOF", ok() goes false, and current() is null.  however 
                           advance() can still be retries as a reactivation attempt.  when there is new data, it will 
                           return true.  that's what we are doing here.
                           */
                        if ( c->advance() )
                            continue;

                        if( n == 0 && (queryOptions & QueryOption_AwaitData) && pass < 1000 ) {
                            throw GetMoreWaitException();
                        }

                        break;
                    }
                    p.release();
                    bool ok = ClientCursor::erase(cursorid);
                    assert(ok);
                    cursorid = 0;
                    cc = 0;
                    break;
                }
                // in some cases (clone collection) there won't be a matcher
                if ( c->matcher() && !c->matcher()->matches(c->currKey(), c->currLoc() ) ) {
                }
                /*
                  TODO
                else if ( _chunkMatcher && ! _chunkMatcher->belongsToMe( c->currKey(), c->currLoc() ) ){
                    cout << "TEMP skipping un-owned chunk: " << c->current() << endl;
                }
                */
                else {
                    if( c->getsetdup(c->currLoc()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        last = c->currLoc();
                        BSONObj js = c->current();

                        // show disk loc should be part of the main query, not in an $or clause, so this should be ok
                        fillQueryResultFromObj(b, cc->fields.get(), js, ( cc->pq.get() && cc->pq->showDiskLoc() ? &last : 0));
                        n++;
                        if ( (ntoreturn>0 && (n >= ntoreturn || b.len() > MaxBytesToReturnToClientAtOnce)) ||
                             (ntoreturn==0 && b.len()>1*1024*1024) ) {
                            c->advance();
                            cc->pos += n;
                            break;
                        }
                    }
                }
                c->advance();
            }
            
            if ( cc ) {
                cc->updateLocation();
                cc->mayUpgradeStorage();
                cc->storeOpForSlave( last );
                exhaust = cc->_queryOptions & QueryOption_Exhaust;
            }
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->_resultFlags() = resultFlags;
        qr->cursorId = cursorid;
        qr->startingFrom = start;
        qr->nReturned = n;
        b.decouple();

        return qr;
    }

    class CountOp : public QueryOp {
    public:
        CountOp( const string& ns , const BSONObj &spec ) :
            _ns(ns), count_(), _myCount(),
            skip_( spec["skip"].numberLong() ),
            limit_( spec["limit"].numberLong() ),
            bc_(){
        }
        
        virtual void _init() {
            c_ = qp().newCursor();
            
            if ( qp().exactKeyMatch() && ! matcher()->needRecord() ) {
                query_ = qp().simplifiedQuery( qp().indexKey() );
                bc_ = dynamic_cast< BtreeCursor* >( c_.get() );
                bc_->forgetEndKey();
            }
        }

        virtual long long nscanned() {
            assert( c_.get() );
            return c_->nscanned();
        }
        
        virtual bool prepareToYield() {
            if ( ! _cc ) {
                _cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , c_ , _ns.c_str() ) );
            }
            return _cc->prepareToYield( _yieldData );
        }
        
        virtual void recoverFromYield() {
            if ( !ClientCursor::recoverFromYield( _yieldData ) ) {
                c_.reset();
                _cc.reset();
                massert( 13337, "cursor dropped during count", false );
                // TODO maybe we want to prevent recording the winning plan as well?
            }
        }
        
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }

            if ( bc_ ) {
                if ( firstMatch_.isEmpty() ) {
                    firstMatch_ = bc_->currKeyNode().key;
                    // if not match
                    if ( query_.woCompare( firstMatch_, BSONObj(), false ) ) {
                        setComplete();
                        return;
                    }
                    _gotOne();
                } else {
                    if ( !firstMatch_.woEqual( bc_->currKeyNode().key ) ) {
                        setComplete();
                        return;
                    }
                    _gotOne();
                }
            } 
            else {
                if ( !matcher()->matches(c_->currKey(), c_->currLoc() ) ) {
                }
                else if( !c_->getsetdup(c_->currLoc()) ) {
                    _gotOne();
                }                
            }
            c_->advance();
        }
        virtual QueryOp *_createChild() const {
            CountOp *ret = new CountOp( _ns , BSONObj() );
            ret->count_ = count_;
            ret->skip_ = skip_;
            ret->limit_ = limit_;
            return ret;
        }
        long long count() const { return count_; }
        virtual bool mayRecordPlan() const {
            return ( _myCount > limit_ / 2 ) || ( complete() && !stopRequested() );
        }
    private:
        
        void _gotOne(){
            if ( skip_ ){
                skip_--;
                return;
            }
            
            if ( limit_ > 0 && count_ >= limit_ ){
                setStop();
                return;
            }

            count_++;
            _myCount++;
        }

        string _ns;
        
        long long count_;
        long long _myCount;
        long long skip_;
        long long limit_;
        shared_ptr<Cursor> c_;
        BSONObj query_;
        BtreeCursor *bc_;
        BSONObj firstMatch_;

        ClientCursor::CleanupPointer _cc;
        ClientCursor::YieldData _yieldData;
    };

    /* { count: "collectionname"[, query: <query>] }
       returns -1 on ns does not exist error.
    */    
    long long runCount( const char *ns, const BSONObj &cmd, string &err ) {
        Client::Context cx(ns);
        NamespaceDetails *d = nsdetails( ns );
        if ( !d ) {
            err = "ns missing";
            return -1;
        }
        BSONObj query = cmd.getObjectField("query");
        
        // count of all objects
        if ( query.isEmpty() ){
            return applySkipLimit( d->nrecords , cmd );
        }
        MultiPlanScanner mps( ns, query, BSONObj(), 0, true, BSONObj(), BSONObj(), false, true );
        CountOp original( ns , cmd );
        shared_ptr< CountOp > res = mps.runOp( original );
        if ( !res->complete() ) {
            log() << "Count with ns: " << ns << " and query: " << query
                  << " failed with exception: " << res->exception()
                  << endl;
            return 0;
        }
        return res->count();
    }
    
    class ExplainBuilder {
    public:
        ExplainBuilder() : _i() {}
        void ensureStartScan() {
            if ( !_a.get() ) {
                _a.reset( new BSONArrayBuilder() );
            }
        }
        void noteCursor( Cursor *c ) {
            BSONObjBuilder b( _a->subobjStart() );
            b << "cursor" << c->toString() << "indexBounds" << c->prettyIndexBounds();
            b.done();
        }
        void noteScan( Cursor *c, long long nscanned, long long nscannedObjects, int n, bool scanAndOrder, int millis, bool hint ) {
            if ( _i == 1 ) {
                _c.reset( new BSONArrayBuilder() );
                *_c << _b->obj();
            }
            if ( _i == 0 ) {
                _b.reset( new BSONObjBuilder() );
            } else {
                _b.reset( new BSONObjBuilder( _c->subobjStart() ) );
            }
            *_b << "cursor" << c->toString();
            _b->appendNumber( "nscanned", nscanned );
            _b->appendNumber( "nscannedObjects", nscannedObjects );
            *_b << "n" << n;

            if ( scanAndOrder )
                *_b << "scanAndOrder" << true;

            *_b << "millis" << millis;

            *_b << "indexBounds" << c->prettyIndexBounds();

            if ( !hint ) {
                *_b << "allPlans" << _a->arr();
            }
            if ( _i != 0 ) {
                _b->done();
            }
            _a.reset( 0 );
            ++_i;
        }
        BSONObj finishWithSuffix( long long nscanned, long long nscannedObjects, int n, int millis, const BSONObj &suffix ) { 
            if ( _i > 1 ) {
                BSONObjBuilder b;
                b << "clauses" << _c->arr();
                b.appendNumber( "nscanned", nscanned );
                b.appendNumber( "nscanneObjects", nscannedObjects );
                b << "n" << n;
                b << "millis" << millis;
                b.appendElements( suffix );
                return b.obj();
            } else {
                _b->appendElements( suffix );
                return _b->obj();                
            }
        }
    private:
        auto_ptr< BSONArrayBuilder > _a;
        auto_ptr< BSONObjBuilder > _b;
        auto_ptr< BSONArrayBuilder > _c;
        int _i;
    };
    
    // Implements database 'query' requests using the query optimizer's QueryOp interface
    class UserQueryOp : public QueryOp {
    public:
        
        UserQueryOp( const ParsedQuery& pq, Message &response, ExplainBuilder &eb, CurOp &curop ) :
            _buf( 32768 ) , // TODO be smarter here
            _pq( pq ) ,
            _ntoskip( pq.getSkip() ) ,
            _nscanned(0), _oldNscanned(0), _nscannedObjects(0), _oldNscannedObjects(0),
            _n(0),
            _oldN(0),
            _chunkMatcher(shardingState.getChunkMatcher(pq.ns())),
            _inMemSort(false),
            _saveClientCursor(false),
            _wouldSaveClientCursor(false),
            _oplogReplay( pq.hasOption( QueryOption_OplogReplay) ),
            _response( response ),
            _eb( eb ),
            _curop( curop )
        {}
        
        virtual void _init() {
            // only need to put the QueryResult fields there if we're building the first buffer in the message.
            if ( _response.empty() ) {
                _buf.skip( sizeof( QueryResult ) );
            }
            
            if ( _oplogReplay ) {
                _findingStartCursor.reset( new FindingStartCursor( qp() ) );
            } else {
                _c = qp().newCursor( DiskLoc() , _pq.getNumToReturn() + _pq.getSkip() );
            }

            if ( qp().scanAndOrderRequired() ) {
                _inMemSort = true;
                _so.reset( new ScanAndOrder( _pq.getSkip() , _pq.getNumToReturn() , _pq.getOrder() ) );
            }
            
            if ( _pq.isExplain() ) {
                _eb.noteCursor( _c.get() );
            }
        }
        
        virtual bool prepareToYield() {
            if ( _findingStartCursor.get() ) {
                return _findingStartCursor->prepareToYield();
            } else {
                if ( ! _cc ) {
                    _cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , _c , _pq.ns() ) );
                }
                return _cc->prepareToYield( _yieldData );
            }
        }
        
        virtual void recoverFromYield() {
            if ( _findingStartCursor.get() ) {
                _findingStartCursor->recoverFromYield();
            } else {
                if ( !ClientCursor::recoverFromYield( _yieldData ) ) {
                    _c.reset();
                    _cc.reset();
                    _so.reset();
                    massert( 13338, "cursor dropped during query", false );
                    // TODO maybe we want to prevent recording the winning plan as well?
                } 
            }
        }
        
        virtual long long nscanned() {
            if ( _findingStartCursor.get() ) {
                return 0; // should only be one query plan, so value doesn't really matter.
            }
            assert( _c.get() );
            return _c->nscanned();
        }
        
        virtual void next() {
            if ( _findingStartCursor.get() ) {
                if ( _findingStartCursor->done() ) {
                    _c = _findingStartCursor->cRelease();
                    _findingStartCursor.reset( 0 );
                } else {
                    _findingStartCursor->next();
                }
                return;
            }
            
            if ( !_c->ok() ) {
                finish( false );
                return;
            }

            bool mayCreateCursor1 = _pq.wantMore() && ! _inMemSort && _pq.getNumToReturn() != 1 && useCursors;
            
            if( 0 ) { 
                cout << "SCANNING this: " << this << " key: " << _c->currKey() << " obj: " << _c->current() << endl;
            }
            
            if ( _pq.getMaxScan() && _nscanned >= _pq.getMaxScan() ){
                finish( true ); //?
                return;
            }

            _nscanned = _c->nscanned();
            if ( !matcher()->matches(_c->currKey(), _c->currLoc() , &_details ) ) {
                // not a match, continue onward
                if ( _details.loadedObject )
                    _nscannedObjects++;
            }
            else {
                _nscannedObjects++;
                DiskLoc cl = _c->currLoc();
                if ( _chunkMatcher && ! _chunkMatcher->belongsToMe( _c->currKey(), _c->currLoc() ) ){
                    // cout << "TEMP skipping un-owned chunk: " << _c->current() << endl;
                }
                else if( _c->getsetdup(cl) ) { 
                    // dup
                }
                else {
                    // got a match.
                    
                    if ( _inMemSort ) {
                        // note: no cursors for non-indexed, ordered results.  results must be fairly small.
                        _so->add( _pq.returnKey() ? _c->currKey() : _c->current(), _pq.showDiskLoc() ? &cl : 0 );
                    }
                    else if ( _ntoskip > 0 ) {
                        _ntoskip--;
                    } 
                    else {
                        if ( _pq.isExplain() ) {
                            _n++;
                            if ( n() >= _pq.getNumToReturn() && !_pq.wantMore() ) {
                                // .limit() was used, show just that much.
                                finish( true ); //?
                                return;
                            }
                        }
                        else {

                            if ( _pq.returnKey() ){
                                BSONObjBuilder bb( _buf );
                                bb.appendKeys( _c->indexKeyPattern() , _c->currKey() );
                                bb.done();
                            }
                            else {
                                BSONObj js = _c->current();
                                assert( js.isValid() );

                                if ( _oplogReplay ){
                                    BSONElement e = js["ts"];
                                    if ( e.type() == Date || e.type() == Timestamp )
                                        _slaveReadTill = e._opTime();
                                }

                                fillQueryResultFromObj( _buf , _pq.getFields() , js , (_pq.showDiskLoc() ? &cl : 0));
                            }
                            _n++;
                            if ( ! _c->supportGetMore() ){
                                if ( _pq.enough( n() ) || _buf.len() >= MaxBytesToReturnToClientAtOnce ){
                                    finish( true );
                                    return;
                                }
                            }
                            else if ( _pq.enoughForFirstBatch( n() , _buf.len() ) ){
                                /* if only 1 requested, no cursor saved for efficiency...we assume it is findOne() */
                                if ( mayCreateCursor1 ) {
                                    _wouldSaveClientCursor = true;
                                    if ( _c->advance() ) {
                                        // more...so save a cursor
                                        _saveClientCursor = true;
                                    }
                                }
                                finish( true );
                                return;
                            }
                        }
                    }
                }
            }
            _c->advance();            
        }

        // this plan won, so set data for response broadly
        void finish( bool stop ) {
            if ( _c.get() ) {
                _nscanned = _c->nscanned();
            }
            if ( _pq.isExplain() ) {
                _n = _inMemSort ? _so->size() : _n;
            } 
            else if ( _inMemSort ) {
                if( _so.get() )
                    _so->fill( _buf, _pq.getFields() , _n );
            }
            
            if ( _pq.hasOption( QueryOption_CursorTailable ) && _pq.getNumToReturn() != 1 )
                _c->setTailable();
            
            // If the tailing request succeeded.
            if ( _c->tailable() )
                _saveClientCursor = true;

            if ( _pq.isExplain()) {
                _eb.noteScan( _c.get(), _nscanned, _nscannedObjects, _n, scanAndOrderRequired(), _curop.elapsedMillis(), useHints && !_pq.getHint().eoo() );
            } else {
                _response.appendData( _buf.buf(), _buf.len() );
                _buf.decouple();
            }
            if ( stop ) {
                setStop();
            } else {
                setComplete();
            }

        }
        
        void finishExplain( const BSONObj &suffix ) {
            BSONObj obj = _eb.finishWithSuffix( totalNscanned(), nscannedObjects(), n(), _curop.elapsedMillis(), suffix);
            fillQueryResultFromObj(_buf, 0, obj);
            _n = 1;
            _oldN = 0;
            _response.appendData( _buf.buf(), _buf.len() );
            _buf.decouple();
        }
        
        virtual bool mayRecordPlan() const {
            return ( _pq.getNumToReturn() != 1 ) && ( ( _n > _pq.getNumToReturn() / 2 ) || ( complete() && !stopRequested() ) );
        }
        
        virtual QueryOp *_createChild() const {
            if ( _pq.isExplain() ) {
                _eb.ensureStartScan();
            }
            UserQueryOp *ret = new UserQueryOp( _pq, _response, _eb, _curop );
            ret->_oldN = n();
            ret->_oldNscanned = totalNscanned();
            ret->_oldNscannedObjects = nscannedObjects();
            ret->_ntoskip = _ntoskip;
            return ret;
        }

        bool scanAndOrderRequired() const { return _inMemSort; }
        shared_ptr<Cursor> cursor() { return _c; }
        int n() const { return _oldN + _n; }
        long long totalNscanned() const { return _nscanned + _oldNscanned; }
        long long nscannedObjects() const { return _nscannedObjects + _oldNscannedObjects; }
        bool saveClientCursor() const { return _saveClientCursor; }
        bool wouldSaveClientCursor() const { return _wouldSaveClientCursor; }
        
        void finishForOplogReplay( ClientCursor * cc ){
            if ( _oplogReplay && ! _slaveReadTill.isNull() )
                cc->_slaveReadTill = _slaveReadTill;

        }
    private:
        BufBuilder _buf;
        const ParsedQuery& _pq;

        long long _ntoskip;
        long long _nscanned;
        long long _oldNscanned;
        long long _nscannedObjects;
        long long _oldNscannedObjects;
        int _n; // found so far
        int _oldN;
        
        MatchDetails _details;

        ChunkMatcherPtr _chunkMatcher;
        
        bool _inMemSort;
        auto_ptr< ScanAndOrder > _so;
        
        shared_ptr<Cursor> _c;
        ClientCursor::CleanupPointer _cc;
        ClientCursor::YieldData _yieldData;

        bool _saveClientCursor;
        bool _wouldSaveClientCursor;
        bool _oplogReplay;
        auto_ptr< FindingStartCursor > _findingStartCursor;
        
        Message &_response;
        ExplainBuilder &_eb;
        CurOp &_curop;
        OpTime _slaveReadTill;
    };
    
    /* run a query -- includes checking for and running a Command */
    const char *runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result) {
        StringBuilder& ss = curop.debug().str;
        shared_ptr<ParsedQuery> pq_shared( new ParsedQuery(q) );
        ParsedQuery& pq( *pq_shared );
        int ntoskip = q.ntoskip;
        BSONObj jsobj = q.query;
        int queryOptions = q.queryOptions;
        const char *ns = q.ns;
        
        if( logLevel >= 2 )
            log() << "query: " << ns << jsobj << endl;
        
        ss << ns;
        {
            // only say ntoreturn if nonzero. 
            int n =  pq.getNumToReturn();
            if( n ) 
                ss << " ntoreturn:" << n;
        }
        curop.setQuery(jsobj);
        
        if ( pq.couldBeCommand() ) {
            BufBuilder bb;
            bb.skip(sizeof(QueryResult));
            BSONObjBuilder cmdResBuf;
            if ( runCommands(ns, jsobj, curop, bb, cmdResBuf, false, queryOptions) ) {
                ss << " command: ";
                jsobj.toString( ss );
                curop.markCommand();
                auto_ptr< QueryResult > qr;
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                ss << " reslen:" << bb.len();
                qr->setOperation(opReply);
                qr->cursorId = 0;
                qr->startingFrom = 0;
                qr->nReturned = 1;
                result.setData( qr.release(), true );
            }
            return false;
        }
        
        /* --- regular query --- */

        int n = 0;
        BSONElement hint = useHints ? pq.getHint() : BSONElement();
        bool explain = pq.isExplain();
        bool snapshot = pq.isSnapshot();
        BSONObj order = pq.getOrder();
        BSONObj query = pq.getFilter();

        /* The ElemIter will not be happy if this isn't really an object. So throw exception
           here when that is true.
           (Which may indicate bad data from client.)
        */
        if ( query.objsize() == 0 ) {
            out() << "Bad query object?\n  jsobj:";
            out() << jsobj.toString() << "\n  query:";
            out() << query.toString() << endl;
            uassert( 10110 , "bad query object", false);
        }
            
        /* --- read lock --- */

        mongolock lk(false);

        Client::Context ctx( ns , dbpath , &lk );

        replVerifyReadsOk(pq);

        if ( pq.hasOption( QueryOption_CursorTailable ) ) {
            NamespaceDetails *d = nsdetails( ns );
            uassert( 13051, "tailable cursor requested on non capped collection", d && d->capped );
            const BSONObj nat1 = BSON( "$natural" << 1 );
            if ( order.isEmpty() ) {
                order = nat1;
            } else {
                uassert( 13052, "only {$natural:1} order allowed for tailable cursor", order == nat1 );
            }
        }
        
        BSONObj snapshotHint; // put here to keep the data in scope
        if( snapshot ) { 
            NamespaceDetails *d = nsdetails(ns);
            if ( d ){
                int i = d->findIdIndex();
                if( i < 0 ) { 
                    if ( strstr( ns , ".system." ) == 0 )
                        log() << "warning: no _id index on $snapshot query, ns:" << ns << endl;
                }
                else {
                    /* [dm] the name of an _id index tends to vary, so we build the hint the hard way here.
                       probably need a better way to specify "use the _id index" as a hint.  if someone is
                       in the query optimizer please fix this then!
                    */
                    BSONObjBuilder b;
                    b.append("$hint", d->idx(i).indexName());
                    snapshotHint = b.obj();
                    hint = snapshotHint.firstElement();
                }
            }
        }
            
        if ( ! (explain || pq.showDiskLoc()) && isSimpleIdQuery( query ) && !pq.hasOption( QueryOption_CursorTailable ) ) {
            bool nsFound = false;
            bool indexFound = false;

            BSONObj resObject;
            Client& c = cc();
            bool found = Helpers::findById( c, ns , query , resObject , &nsFound , &indexFound );
            if ( nsFound == false || indexFound == true ){
                BufBuilder bb(sizeof(QueryResult)+resObject.objsize()+32);
                bb.skip(sizeof(QueryResult));
                
                ss << " idhack ";
                if ( found ){
                    n = 1;
                    fillQueryResultFromObj( bb , pq.getFields() , resObject );
                }
                auto_ptr< QueryResult > qr;
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                ss << " reslen:" << bb.len();
                qr->setOperation(opReply);
                qr->cursorId = 0;
                qr->startingFrom = 0;
                qr->nReturned = n;      
                result.setData( qr.release(), true );
                return false;
            }     
        }
        
        // regular, not QO bypass query
        
        BSONObj oldPlan;
        if ( explain && ! pq.hasIndexSpecifier() ){
            MultiPlanScanner mps( ns, query, order );
            if ( mps.usingPrerecordedPlan() )
                oldPlan = mps.oldExplain();
        }
        auto_ptr< MultiPlanScanner > mps( new MultiPlanScanner( ns, query, order, &hint, !explain, pq.getMin(), pq.getMax(), false, true ) );
        BSONObj explainSuffix;
        if ( explain ) {
            BSONObjBuilder bb;
            if ( !oldPlan.isEmpty() )
                bb.append( "oldPlan", oldPlan.firstElement().embeddedObject().firstElement().embeddedObject() );
            explainSuffix = bb.obj();
        }
        ExplainBuilder eb;
        UserQueryOp original( pq, result, eb, curop );
        shared_ptr< UserQueryOp > o = mps->runOp( original );
        UserQueryOp &dqo = *o;
        if ( ! dqo.complete() )
            throw MsgAssertionException( dqo.exception() );
        if ( explain ) {
            dqo.finishExplain( explainSuffix );
        }
        n = dqo.n();
        long long nscanned = dqo.totalNscanned();
        if ( dqo.scanAndOrderRequired() )
            ss << " scanAndOrder ";
        shared_ptr<Cursor> cursor = dqo.cursor();
        if( logLevel >= 5 )
            log() << "   used cursor: " << cursor.get() << endl;
        long long cursorid = 0;
        const char * exhaust = 0;
        if ( dqo.saveClientCursor() || ( dqo.wouldSaveClientCursor() && mps->mayRunMore() ) ) {
            ClientCursor *cc;
            bool moreClauses = mps->mayRunMore();
            if ( moreClauses ) {
                // this MultiCursor will use a dumb NoOp to advance(), so no need to specify mayYield
                shared_ptr< Cursor > multi( new MultiCursor( mps, cursor, dqo.matcher(), dqo ) );
                cc = new ClientCursor(queryOptions, multi, ns, jsobj.getOwned());
            } else {
                cursor->setMatcher( dqo.matcher() );
                cc = new ClientCursor( queryOptions, cursor, ns, jsobj.getOwned() );
            }
            cursorid = cc->cursorid;
            DEV tlog(2) << "query has more, cursorid: " << cursorid << endl;
            cc->pos = n;
            cc->pq = pq_shared;
            cc->fields = pq.getFieldPtr();
            cc->originalMessage = m;
            cc->updateLocation();
            if ( !cc->c->ok() && cc->c->tailable() )
                DEV tlog() << "query has no more but tailable, cursorid: " << cursorid << endl;
            if( queryOptions & QueryOption_Exhaust ) {
                exhaust = ns;
                ss << " exhaust ";
            }
            dqo.finishForOplogReplay(cc);
        }

        QueryResult *qr = (QueryResult *) result.header();
        qr->cursorId = cursorid;
        qr->setResultFlagsToOk();
        // qr->len is updated automatically by appendData()
        ss << " reslen:" << qr->len;
        qr->setOperation(opReply);
        qr->startingFrom = 0;
        qr->nReturned = n;

        int duration = curop.elapsedMillis();
        bool dbprofile = curop.shouldDBProfile( duration );
        if ( dbprofile || duration >= cmdLine.slowMS ) {
            ss << " nscanned:" << nscanned << ' ';
            if ( ntoskip )
                ss << " ntoskip:" << ntoskip;
            if ( dbprofile )
                ss << " \nquery: ";
            ss << jsobj.toString() << ' ';
        }
        ss << " nreturned:" << n;
        return exhaust;
    }    
    
} // namespace mongo
