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
    class DeleteOp : public QueryOp {
    public:
        DeleteOp( bool justOne, int& bestCount ) :
            justOne_( justOne ),
            count_(),
            bestCount_( bestCount ),
            _nscanned() {
        }
        virtual void init() {
            c_ = qp().newCursor();
            // FIXME originalQuery won't work for $or
            _matcher.reset( new CoveredIndexMatcher( qp().query(), qp().indexKey() ) );
        }
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }
            
            DiskLoc rloc = c_->currLoc();
            
            if ( _matcher->matches(c_->currKey(), rloc ) ) {
                if ( !c_->getsetdup(rloc) )
                    ++count_;
            }

            c_->advance();
            ++_nscanned;
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
        virtual QueryOp *clone() const {
            return new DeleteOp( justOne_, bestCount_ );
        }
        shared_ptr<Cursor> newCursor() const { return qp().newCursor(); }
    private:
        bool justOne_;
        int count_;
        int &bestCount_;
        long long _nscanned;
        shared_ptr<Cursor> c_;
        auto_ptr< CoveredIndexMatcher > _matcher;
    };
    
    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
       god:     allow access to system namespaces, and don't yield
    */
    long long deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool logop, bool god) {
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
        MultiPlanScanner s( ns, pattern, BSONObj() );
        while( s.mayRunMore() ) {
            int best = 0;
            DeleteOp original( justOne, best );
            shared_ptr< DeleteOp > bestOp = s.runOpOnce( original );
            shared_ptr<Cursor> creal = bestOp->newCursor();
            
            if( !creal->ok() )
                return nDeleted;
            
            CoveredIndexMatcher matcher(pattern, creal->indexKeyPattern());
            
            auto_ptr<ClientCursor> cc( new ClientCursor(QueryOption_NoCursorTimeout, creal, ns) );
            cc->setDoingDeletes( true );
            
            CursorId id = cc->cursorid;
            
            unsigned long long nScanned = 0;
            do {
                if ( ++nScanned % 128 == 0 && !god && !matcher.docMatcher().atomic() ) {
                    if ( ! cc->yield() ){
                        cc.release(); // has already been deleted elsewhere
                        break;
                    }
                }
                
                // this way we can avoid calling updateLocation() every time (expensive)
                // as well as some other nuances handled
                cc->setDoingDeletes( true );
                
                DiskLoc rloc = cc->c->currLoc();
                BSONObj key = cc->c->currKey();
                
                cc->c->advance();
                
                if ( ! matcher.matches( key , rloc ) )
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
            if ( justOne && nDeleted ) {
                break;
            }
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
            if ( !e.msg.empty() )
                anObjBuilder.append("assertion", e.msg);
        }
        curop.debug().str << " assertion ";
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.append((void*) x.objdata(), x.objsize());
        return true;
    }

    int nCaught = 0;

    void killCursors(int n, long long *ids) {
        int k = 0;
        for ( int i = 0; i < n; i++ ) {
            if ( ClientCursor::erase(ids[i]) )
                k++;
        }
        log( k == n ) << "killcursors: found " << k << " of " << n << '\n';
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

    QueryResult* processGetMore(const char *ns, int ntoreturn, long long cursorid , CurOp& curop, int pass ) {

        ClientCursor::Pointer p(cursorid);
        ClientCursor *cc = p._c;
        
        int bufSize = 512;
        if ( cc ){
            bufSize += sizeof( QueryResult );
            bufSize += ( ntoreturn ? 4 : 1 ) * 1024 * 1024;
        }

        BufBuilder b( bufSize );

        b.skip(sizeof(QueryResult));
        
        int resultFlags = QueryResult::ResultFlag_AwaitCapable;
        int start = 0;
        int n = 0;

        if ( !cc ) {
            log() << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = QueryResult::ResultFlag_CursorNotFound;
        }
        else {
            if ( pass == 0 )
                cc->updateSlaveLocation( curop );

            int queryOptions = cc->_queryOptions;

            if( pass == 0 ) {
                StringBuilder& ss = curop.debug().str;
                ss << " getMore: " << cc->query << " ";
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
                if ( !cc->matcher->matches(c->currKey(), c->currLoc() ) ) {
                }
                else {
                    //out() << "matches " << c->currLoc().toString() << '\n';
                    if( c->getsetdup(c->currLoc()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        last = c->currLoc();
                        BSONObj js = c->current();

                        fillQueryResultFromObj(b, cc->fields.get(), js);
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
        CountOp( const BSONObj &spec ) :
        count_(),
        skip_( spec["skip"].numberLong() ),
        limit_( spec["limit"].numberLong() ),
        bc_() {}
        
        virtual void init() {
            query_ = qp().query();
            c_ = qp().newCursor();
            
            log() << "query: " << query_ << endl;
            log() << "index: " << c_->toString() << endl;
            
            _matcher.reset( new CoveredIndexMatcher( query_, c_->indexKeyPattern() ) );
            if ( qp().exactKeyMatch() && ! _matcher->needRecord() ) {
                query_ = qp().simplifiedQuery( qp().indexKey() );
                bc_ = dynamic_cast< BtreeCursor* >( c_.get() );
                bc_->forgetEndKey();
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
            } else {
                if ( !_matcher->matches(c_->currKey(), c_->currLoc() ) ) {
                }
                else if( !c_->getsetdup(c_->currLoc()) ) {
                    _gotOne();
                }                
            }
            c_->advance();
        }
        virtual QueryOp *clone() const {
            CountOp *ret = new CountOp( BSONObj() );
            ret->count_ = count_;
            ret->skip_ = skip_;
            ret->limit_ = limit_;
            return ret;
        }
        long long count() const { return count_; }
        virtual bool mayRecordPlan() const { return true; }
    private:
        
        void _gotOne(){
            if ( skip_ ){
                skip_--;
                return;
            }
            
            if ( limit_ > 0 && count_ >= limit_ ){
                setComplete();
                return;
            }

            count_++;
        }

        long long count_;
        long long skip_;
        long long limit_;
        shared_ptr<Cursor> c_;
        BSONObj query_;
        BtreeCursor *bc_;
        auto_ptr< CoveredIndexMatcher > _matcher;
        BSONObj firstMatch_;
    };
    
    /* { count: "collectionname"[, query: <query>] }
       returns -1 on ns does not exist error.
    */    
    long long runCount( const char *ns, const BSONObj &cmd, string &err ) {
        NamespaceDetails *d = nsdetails( ns );
        if ( !d ) {
            err = "ns missing";
            return -1;
        }
        BSONObj query = cmd.getObjectField("query");

        // count of all objects
        if ( query.isEmpty() ){
            long long num = d->nrecords;
            num = num - cmd["skip"].numberLong();
            if ( num < 0 ) {
                num = 0;
            }
            if ( cmd["limit"].isNumber() ){
                long long limit = cmd["limit"].numberLong();
                if ( limit < num ){
                    num = limit;
                }
            }
            return num;
        }
        MultiPlanScanner qps( ns, query, BSONObj() );
        CountOp original( cmd );
        shared_ptr< CountOp > res = qps.runOp( original );
        if ( !res->complete() ) {
            log() << "Count with ns: " << ns << " and query: " << query
                  << " failed with exception: " << res->exceptionMessage()
                  << endl;
            return 0;
        }
        return res->count();
    }

    // Implements database 'query' requests using the query optimizer's QueryOp interface
    class UserQueryOp : public QueryOp {
    public:
        
        UserQueryOp( const ParsedQuery& pq ) :
        //int ntoskip, int ntoreturn, const BSONObj &order, bool wantMore,
        //                   bool explain, FieldMatcher *filter, int queryOptions ) :
            _buf( 32768 ) , // TODO be smarter here
            _pq( pq ) ,
            _ntoskip( pq.getSkip() ) ,
            _nscanned(0), _nscannedObjects(0),
            _n(0),
            _inMemSort(false),
            _saveClientCursor(false),
            _oplogReplay( pq.hasOption( QueryOption_OplogReplay) )
        {}
        
        virtual void init() {
            _buf.skip( sizeof( QueryResult ) );
            
            if ( _oplogReplay ) {
                _findingStartCursor.reset( new FindingStartCursor( qp() ) );
            } else {
                _c = qp().newCursor( DiskLoc() , _pq.getNumToReturn() + _pq.getSkip() );
            }
            // FIXME get query right way
            _matcher.reset(new CoveredIndexMatcher( qp().query() , qp().indexKey()));

            if ( qp().scanAndOrderRequired() ) {
                _inMemSort = true;
                _so.reset( new ScanAndOrder( _pq.getSkip() , _pq.getNumToReturn() , _pq.getOrder() ) );
            }
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
                finish();
                return;
            }
            
            bool mayCreateCursor1 = _pq.wantMore() && ! _inMemSort && _pq.getNumToReturn() != 1 && useCursors;
            
            if( 0 ) { 
                cout << "SCANNING this: " << this << " key: " << _c->currKey() << " obj: " << _c->current() << endl;
            }
            
            if ( _pq.getMaxScan() && _nscanned >= _pq.getMaxScan() ){
                finish();
                return;
            }

            _nscanned++;
            if ( !_matcher->matches(_c->currKey(), _c->currLoc() , &_details ) ) {
                // not a match, continue onward
                if ( _details.loadedObject )
                    _nscannedObjects++;
            }
            else {
                _nscannedObjects++;
                DiskLoc cl = _c->currLoc();
                if( !_c->getsetdup(cl) ) { 
                    // got a match.
                    
                    if ( _inMemSort ) {
                        // note: no cursors for non-indexed, ordered results.  results must be fairly small.
                        _so->add( _pq.returnKey() ? _c->currKey() : _c->current() );
                    }
                    else if ( _ntoskip > 0 ) {
                        _ntoskip--;
                    } 
                    else {
                        if ( _pq.isExplain() ) {
                            _n++;
                            if ( _n >= _pq.getNumToReturn() && !_pq.wantMore() ) {
                                // .limit() was used, show just that much.
                                finish();
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
                                fillQueryResultFromObj( _buf , _pq.getFields() , js );
                            }
                            _n++;
                            if ( ! _c->supportGetMore() ){
                                if ( _pq.enough( _n ) || _buf.len() >= MaxBytesToReturnToClientAtOnce ){
                                    finish();
                                    return;
                                }
                            }
                            else if ( _pq.enoughForFirstBatch( _n , _buf.len() ) ){
                                /* if only 1 requested, no cursor saved for efficiency...we assume it is findOne() */
                                if ( mayCreateCursor1 ) {
                                    _c->advance();
                                    if ( _c->ok() ) {
                                        // more...so save a cursor
                                        _saveClientCursor = true;
                                    }
                                }
                                finish();
                                return;
                            }
                        }
                    }
                }
            }
            _c->advance();            
        }

        void finish() {
            if ( _pq.isExplain() ) {
                _n = _inMemSort ? _so->size() : _n;
            } 
            else if ( _inMemSort ) {
                _so->fill( _buf, _pq.getFields() , _n );
            }
            
            if ( _pq.hasOption( QueryOption_CursorTailable ) && _pq.getNumToReturn() != 1 )
                _c->setTailable();
            
            // If the tailing request succeeded.
            if ( _c->tailable() )
                _saveClientCursor = true;

            setComplete();            
        }
        
        virtual bool mayRecordPlan() const { return _pq.getNumToReturn() != 1; }
        
        virtual QueryOp *clone() const {
            return new UserQueryOp( _pq );
        }

        BufBuilder &builder() { return _buf; }
        bool scanAndOrderRequired() const { return _inMemSort; }
        shared_ptr<Cursor> cursor() { return _c; }
        auto_ptr< CoveredIndexMatcher > matcher() { return _matcher; }
        int n() const { return _n; }
        long long nscanned() const { return _nscanned; }
        long long nscannedObjects() const { return _nscannedObjects; }
        bool saveClientCursor() const { return _saveClientCursor; }

    private:
        BufBuilder _buf;
        const ParsedQuery& _pq;

        long long _ntoskip;
        long long _nscanned;
        long long _nscannedObjects;
        int _n; // found so far
        
        MatchDetails _details;

        bool _inMemSort;
        auto_ptr< ScanAndOrder > _so;
        
        shared_ptr<Cursor> _c;

        auto_ptr< CoveredIndexMatcher > _matcher;

        bool _saveClientCursor;
        bool _oplogReplay;
        auto_ptr< FindingStartCursor > _findingStartCursor;
    };
    
    /* run a query -- includes checking for and running a Command */
    auto_ptr< QueryResult > runQuery(Message& m, QueryMessage& q, CurOp& curop ) {
        StringBuilder& ss = curop.debug().str;
        ParsedQuery pq( q );
        const char *ns = q.ns;
        int ntoskip = q.ntoskip;
        BSONObj jsobj = q.query;
        int queryOptions = q.queryOptions;
        BSONObj snapshotHint;
        
        if( logLevel >= 2 )
            log() << "query: " << ns << jsobj << endl;
        
        long long nscanned = 0;
        ss << ns;
        {
            // only insert if nonzero. 
            int n =  pq.getNumToReturn();
            if( n ) 
                ss << " ntoreturn:" << n;
        }
        curop.setQuery(jsobj);
        
        BSONObjBuilder cmdResBuf;
        long long cursorid = 0;
        
        auto_ptr< QueryResult > qr;
        int n = 0;
        
        Client& c = cc();

        if ( pq.couldBeCommand() ){
            BufBuilder bb;
            bb.skip(sizeof(QueryResult));

            if ( runCommands(ns, jsobj, curop, bb, cmdResBuf, false, queryOptions) ) {
                ss << " command: " << jsobj;
                curop.markCommand();
                n = 1;
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                ss << " reslen:" << bb.len();
                //	qr->channel = 0;
                qr->setOperation(opReply);
                qr->cursorId = cursorid;
                qr->startingFrom = 0;
                qr->nReturned = n;
            }
            return qr;
        }
        
        // regular query

        mongolock lk(false); // read lock
        Client::Context ctx( ns , dbpath , &lk );

        replVerifyReadsOk(pq);
        //uassert( ?? , "not master" , isMaster() || pq.hasOption( QueryOption_SlaveOk ) || replSettings.slave == SimpleSlave );

        BSONElement hint = useHints ? pq.getHint() : BSONElement();
        bool explain = pq.isExplain();
        bool snapshot = pq.isSnapshot();
        BSONObj query = pq.getFilter();
        BSONObj order = pq.getOrder();

        if ( pq.hasOption( QueryOption_CursorTailable ) ) {
            NamespaceDetails *d = nsdetails( ns );
            uassert( 13051, "tailable cursor requested on non capped collection", d && d->capped );
            if ( order.isEmpty() ) {
                order = BSON( "$natural" << 1 );
            } else {
                uassert( 13052, "only {$natural:1} order allowed for tailable cursor", order == BSON( "$natural" << 1 ) );
            }
        }
        
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
            
        if ( ! explain && isSimpleIdQuery( query ) && !pq.hasOption( QueryOption_CursorTailable ) ) {
            nscanned = 1;

            bool nsFound = false;
            bool indexFound = false;

            BSONObj resObject;
            bool found = Helpers::findById( c, ns , query , resObject , &nsFound , &indexFound );
            if ( nsFound == false || indexFound == true ){
                BufBuilder bb(sizeof(QueryResult)+resObject.objsize()+32);
                bb.skip(sizeof(QueryResult));
                
                ss << " idhack ";
                if ( found ){
                    n = 1;
                    fillQueryResultFromObj( bb , pq.getFields() , resObject );
                }
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                ss << " reslen:" << bb.len();
                qr->setOperation(opReply);
                qr->cursorId = cursorid;
                qr->startingFrom = 0;
                qr->nReturned = n;       
                return qr;
            }     
        }
        
        // regular, not QO bypass query
        
        BSONObj oldPlan;
        if ( explain && ! pq.hasIndexSpecifier() ){
            MultiPlanScanner qps( ns, query, order );
            if ( qps.usingPrerecordedPlan() )
                oldPlan = qps.explain();
        }
        MultiPlanScanner qps( ns, query, order, &hint, !explain, pq.getMin(), pq.getMax() );
        UserQueryOp original( pq );
        shared_ptr< UserQueryOp > o = qps.runOp( original );
        UserQueryOp &dqo = *o;
        massert( 10362 ,  dqo.exceptionMessage(), dqo.complete() );
        n = dqo.n();
        nscanned = dqo.nscanned();
        if ( dqo.scanAndOrderRequired() )
            ss << " scanAndOrder ";
        shared_ptr<Cursor> cursor = dqo.cursor();
        log( 5 ) << "   used cursor: " << cursor.get() << endl;
        if ( dqo.saveClientCursor() ) {
            // the clientcursor now owns the Cursor* and 'c' is released:
            ClientCursor *cc = new ClientCursor(queryOptions, cursor, ns);
            cursorid = cc->cursorid;
            cc->query = jsobj.getOwned();
            DEV out() << "  query has more, cursorid: " << cursorid << endl;
            cc->matcher = dqo.matcher();
            cc->pos = n;
            cc->fields = pq.getFieldPtr();
            cc->originalMessage = m;
            cc->updateLocation();
            if ( !cc->c->ok() && cc->c->tailable() ) {
                DEV out() << "  query has no more but tailable, cursorid: " << cursorid << endl;
            } else {
                DEV out() << "  query has more, cursorid: " << cursorid << endl;
            }
        }
        if ( explain ) {
            BSONObjBuilder builder;
            builder.append("cursor", cursor->toString());
            builder.appendArray("indexBounds", cursor->prettyIndexBounds());
            builder.appendNumber("nscanned", dqo.nscanned() );
            builder.appendNumber("nscannedObjects", dqo.nscannedObjects() );
            builder.append("n", n);
            if ( dqo.scanAndOrderRequired() )
                builder.append("scanAndOrder", true);
            builder.append("millis", curop.elapsedMillis());
            if ( !oldPlan.isEmpty() )
                builder.append( "oldPlan", oldPlan.firstElement().embeddedObject().firstElement().embeddedObject() );
            if ( hint.eoo() )
                builder.appendElements(qps.explain());
            BSONObj obj = builder.done();
            fillQueryResultFromObj(dqo.builder(), 0, obj);
            n = 1;
        }
        qr.reset( (QueryResult *) dqo.builder().buf() );
        dqo.builder().decouple();
        qr->cursorId = cursorid;
        qr->setResultFlagsToOk();
        qr->len = dqo.builder().len();
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
            ss << jsobj << ' ';
        }
        ss << " nreturned:" << n;
        return qr;        
    }    
    
} // namespace mongo
