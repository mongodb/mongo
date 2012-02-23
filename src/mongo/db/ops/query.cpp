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
#include "../pdfile.h"
#include "../jsobjmanipulator.h"
#include "../../bson/util/builder.h"
#include <time.h>
#include "../introspect.h"
#include "../btree.h"
#include "../../util/lruishmap.h"
#include "../json.h"
#include "../repl.h"
#include "../replutil.h"
#include "../scanandorder.h"
#include "../security.h"
#include "../curop-inl.h"
#include "../commands.h"
#include "../queryoptimizer.h"
#include "../lasterror.h"
#include "../../s/d_logic.h"
#include "../repl_block.h"
#include "../../server.h"
#include "../d_concurrency.h"

namespace mongo {

    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    const int MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

    //ns->query->DiskLoc
//    LRUishMap<BSONObj,DiskLoc,5> lrutest(123);

    extern bool useHints;

    bool runCommands(const char *ns, BSONObj& jsobj, CurOp& curop, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        try {
            return _runCommands(ns, jsobj, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch( SendStaleConfigException& ){
            throw;
        }
        catch ( AssertionException& e ) {
            assert( e.getCode() != SendStaleConfigCode && e.getCode() != RecvStaleConfigCode );

            e.getInfo().append( anObjBuilder , "assertion" , "assertionCode" );
            curop.debug().exceptionInfo = e.getInfo();
        }
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.appendBuf((void*) x.objdata(), x.objsize());
        return true;
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
        qr->initializeResultFlags();
        qr->nReturned = 0;
        b.decouple();
        return qr;
    }

    QueryResult* processGetMore(const char *ns, int ntoreturn, long long cursorid , CurOp& curop, int pass, bool& exhaust ) {
        exhaust = false;
        ClientCursor::Pointer p(cursorid);
        ClientCursor *cc = p.c();

        int bufSize = 512 + sizeof( QueryResult ) + MaxBytesToReturnToClientAtOnce;

        BufBuilder b( bufSize );
        b.skip(sizeof(QueryResult));
        int resultFlags = ResultFlag_AwaitCapable;
        int start = 0;
        int n = 0;

        if ( unlikely(!cc) ) {
            LOGSOME << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = ResultFlag_CursorNotFound;
        }
        else {
            // check for spoofing of the ns such that it does not match the one originally there for the cursor
            uassert(14833, "auth error", str::equals(ns, cc->ns().c_str()));

            if ( pass == 0 )
                cc->updateSlaveLocation( curop );

            int queryOptions = cc->queryOptions();
            
            curop.debug().query = cc->query();

            start = cc->pos();
            Cursor *c = cc->c();
            c->checkLocation();
            DiskLoc last;

            scoped_ptr<Projection::KeyOnly> keyFieldsOnly;
            if ( cc->modifiedKeys() == false && cc->isMultiKey() == false && cc->fields )
                keyFieldsOnly.reset( cc->fields->checkKey( cc->indexKeyPattern() ) );

            // This manager may be stale, but it's the state of chunking when the cursor was created.
            ShardChunkManagerPtr manager = cc->getChunkManager();

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
                            return 0;
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
                if ( c->matcher() && !c->matcher()->matchesCurrent( c ) ) {
                }
                else if ( manager && ! manager->belongsToMe( cc ) ){
                    LOG(2) << "cursor skipping document in un-owned chunk: " << c->current() << endl;
                }
                else {
                    if( c->getsetdup(c->currLoc()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        last = c->currLoc();
                        n++;

                        if ( keyFieldsOnly ) {
                            fillQueryResultFromObj(b, 0, keyFieldsOnly->hydrate( c->currKey() ) );
                        }
                        else {
                            BSONObj js = c->current();
                            // show disk loc should be part of the main query, not in an $or clause, so this should be ok
                            fillQueryResultFromObj(b, cc->fields.get(), js, ( cc->pq.get() && cc->pq->showDiskLoc() ? &last : 0));
                        }

                        if ( ( ntoreturn && n >= ntoreturn ) || b.len() > MaxBytesToReturnToClientAtOnce ) {
                            c->advance();
                            cc->incPos( n );
                            break;
                        }
                    }
                }
                c->advance();

                if ( ! cc->yieldSometimes( ClientCursor::MaybeCovered ) ) {
                    ClientCursor::erase(cursorid);
                    cursorid = 0;
                    cc = 0;
                    p.deleted();
                    break;
                }
            }
            
            if ( cc ) {
                cc->updateLocation();
                cc->mayUpgradeStorage();
                cc->storeOpForSlave( last );
                exhaust = cc->queryOptions() & QueryOption_Exhaust;
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

    class ExplainBuilder {
        // Note: by default we filter out allPlans and oldPlan in the shell's
        // explain() function. If you add any recursive structures, make sure to
        // edit the JS to make sure everything gets filtered.
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
        void noteScan( Cursor *c, long long nscanned, long long nscannedObjects, int n, bool scanAndOrder,
                       int millis, bool hint, int nYields , int nChunkSkips , bool indexOnly ) {
            if ( _i == 1 ) {
                _c.reset( new BSONArrayBuilder() );
                *_c << _b->obj();
            }
            if ( _i == 0 ) {
                _b.reset( new BSONObjBuilder() );
            }
            else {
                _b.reset( new BSONObjBuilder( _c->subobjStart() ) );
            }
            *_b << "cursor" << c->toString();
            _b->appendNumber( "nscanned", nscanned );
            _b->appendNumber( "nscannedObjects", nscannedObjects );
            *_b << "n" << n;

            if ( scanAndOrder )
                *_b << "scanAndOrder" << true;

            *_b << "millis" << millis;

            *_b << "nYields" << nYields;
            *_b << "nChunkSkips" << nChunkSkips;
            *_b << "isMultiKey" << c->isMultiKey();
            *_b << "indexOnly" << indexOnly;

            *_b << "indexBounds" << c->prettyIndexBounds();

            c->explainDetails( *_b );

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
                b.appendNumber( "nscannedObjects", nscannedObjects );
                b << "n" << n;
                b << "millis" << millis;
                b.appendElements( suffix );
                return b.obj();
            }
            else {
            	stringstream host;
            	host << getHostNameCached() << ":" << cmdLine.port;
            	*_b << "server" << host.str();
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
            _nYields(),
            _nChunkSkips(),
            _chunkManager( shardingState.needShardChunkManager(pq.ns()) ?
                           shardingState.getShardChunkManager(pq.ns()) : ShardChunkManagerPtr() ),
            _inMemSort(false),
            _capped(false),
            _saveClientCursor(false),
            _wouldSaveClientCursor(false),
            _oplogReplay( pq.hasOption( QueryOption_OplogReplay) ),
            _response( response ),
            _eb( eb ),
            _curop( curop ),
            _yieldRecoveryFailed()
        {}

        virtual void _init() {
            // only need to put the QueryResult fields there if we're building the first buffer in the message.
            if ( _response.empty() ) {
                _buf.skip( sizeof( QueryResult ) );
            }

            if ( _oplogReplay ) {
                _findingStartCursor.reset( new FindingStartCursor( qp() ) );
                _capped = true;
            }
            else {
                _c = qp().newCursor( DiskLoc() , _pq.getNumToReturn() + _pq.getSkip() );
                _capped = _c->capped();

                // setup check for if we can only use index to extract
                if ( _c->modifiedKeys() == false && _c->isMultiKey() == false && _pq.getFields() ) {
                    _keyFieldsOnly.reset( _pq.getFields()->checkKey( _c->indexKeyPattern() ) );
                }
            }

            if ( qp().scanAndOrderRequired() ) {
                _inMemSort = true;
                _so.reset( new ScanAndOrder( _pq.getSkip() , _pq.getNumToReturn() , _pq.getOrder(), qp().multikeyFrs() ) );
            }

            if ( _pq.isExplain() ) {
                _eb.noteCursor( _c.get() );
            }

        }

        virtual bool prepareToYield() {
            if ( _findingStartCursor.get() ) {
                return _findingStartCursor->prepareToYield();
            }
            else {
                if ( _c && !_cc ) {
                    _cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , _c , _pq.ns() ) );
                }
                if ( _cc ) {
	                return _cc->prepareToYield( _yieldData );
                }
            }
            // no active cursor - ok to yield
            return true;
        }

        virtual void recoverFromYield() {
            _nYields++;

            if ( _findingStartCursor.get() ) {
                _findingStartCursor->recoverFromYield();
            }
            else if ( _cc && !ClientCursor::recoverFromYield( _yieldData ) ) {
                _yieldRecoveryFailed = true;
                _c.reset();
                _cc.reset();
                _so.reset();

                if ( _capped ) {
                    msgassertedNoTrace( 13338, str::stream() << "capped cursor overrun during query: " << _pq.ns() );
                }
                else if ( qp().mustAssertOnYieldFailure() ) {
                    msgassertedNoTrace( 15890, str::stream() << "UserQueryOp::recoverFromYield() failed to recover: " << _pq.ns() );
                }
                else {
                    // we don't fail query since we're fine with returning partial data if collection dropped

                    // todo: this is wrong.  the cursor could be gone if closeAllDatabases command just ran
                }

            }
        }

        virtual long long nscanned() {
            if ( _findingStartCursor.get() ) {
                return 0; // should only be one query plan, so value doesn't really matter.
            }
            return _c.get() ? _c->nscanned() : _nscanned;
        }

        virtual void next() {
            if ( _findingStartCursor.get() ) {
                if ( !_findingStartCursor->done() ) {
                    _findingStartCursor->next();
                }                    
                if ( _findingStartCursor->done() ) {
                    _c = _findingStartCursor->cursor();
                    _findingStartCursor.reset( 0 );
                }
                _capped = true;
                return;
            }

            if ( !_c || !_c->ok() ) {
                finish( false );
                return;
            }

            bool mayCreateCursor1 = _pq.wantMore() && ! _inMemSort && _pq.getNumToReturn() != 1;

            if( 0 ) {
                cout << "SCANNING this: " << this << " key: " << _c->currKey() << " obj: " << _c->current() << endl;
            }

            if ( _pq.getMaxScan() && _nscanned >= _pq.getMaxScan() ) {
                finish( true ); //?
                return;
            }

            _nscanned = _c->nscanned();
            if ( !matcher( _c )->matchesCurrent(_c.get() , &_details ) ) {
                // not a match, continue onward
                if ( _details._loadedObject )
                    _nscannedObjects++;
            }
            else {
                _nscannedObjects++;
                DiskLoc cl = _c->currLoc();
                if ( _chunkManager && ! _chunkManager->belongsToMe( cl.obj() ) ) { // TODO: should make this covered at some point
                    _nChunkSkips++;
                    // log() << "TEMP skipping un-owned chunk: " << _c->current() << endl;
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

                            if ( _pq.returnKey() ) {
                                BSONObjBuilder bb( _buf );
                                bb.appendKeys( _c->indexKeyPattern() , _c->currKey() );
                                bb.done();
                            }
                            else if ( _keyFieldsOnly ) {
                                fillQueryResultFromObj( _buf , 0 , _keyFieldsOnly->hydrate( _c->currKey() ) );
                            }
                            else {
                                BSONObj js = _c->current();
                                assert( js.isValid() );

                                if ( _oplogReplay ) {
                                    BSONElement e = js["ts"];
                                    if ( e.type() == Date || e.type() == Timestamp )
                                        _slaveReadTill = e._opTime();
                                }

                                fillQueryResultFromObj( _buf , _pq.getFields() , js , (_pq.showDiskLoc() ? &cl : 0));
                            }
                            _n++;
                            if ( ! _c->supportGetMore() ) {
                                if ( _pq.enough( n() ) || _buf.len() >= MaxBytesToReturnToClientAtOnce ) {
                                    finish( true );
                                    return;
                                }
                            }
                            else if ( _pq.enoughForFirstBatch( n() , _buf.len() ) ) {
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
            massert( 13638, "client cursor dropped during explain query yield", !_pq.isExplain() || _c.get() );

            if ( _pq.isExplain() ) {
                _n = _inMemSort ? _so->size() : _n;
            }
            else if ( _inMemSort ) {
                if( _so.get() )
                    _so->fill( _buf, _pq.getFields() , _n );
            }

            if ( _c.get() ) {
                _nscanned = _c->nscanned();

                if ( _pq.hasOption( QueryOption_CursorTailable ) && _pq.getNumToReturn() != 1 )
                    _c->setTailable();

                // If the tailing request succeeded.
                if ( _c->tailable() )
                    _saveClientCursor = true;
            }

            if ( _pq.isExplain() ) {
                _eb.noteScan( _c.get(), _nscanned, _nscannedObjects, _n, scanAndOrderRequired(),
                              _curop.elapsedMillis(), useHints && !_pq.getHint().isEmpty(), _nYields ,
                              _nChunkSkips, _keyFieldsOnly.get() > 0 );
            }
            else {
                if ( _buf.len() ) {
                    _response.appendData( _buf.buf(), _buf.len() );
                    _buf.decouple();
                }
            }

            if ( stop ) {
                setStop();
            }
            else {
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
            return !_yieldRecoveryFailed && ( _pq.getNumToReturn() != 1 ) && ( ( _n > _pq.getNumToReturn() / 2 ) || ( complete() && !stopRequested() ) );
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

        void finishForOplogReplay( ClientCursor * cc ) {
            if ( _oplogReplay && ! _slaveReadTill.isNull() )
                cc->slaveReadTill( _slaveReadTill );

        }

        ShardChunkManagerPtr getChunkManager(){ return _chunkManager; }

    private:
        BufBuilder _buf;
        const ParsedQuery& _pq;
        scoped_ptr<Projection::KeyOnly> _keyFieldsOnly;

        long long _ntoskip;
        long long _nscanned;
        long long _oldNscanned;
        long long _nscannedObjects;
        long long _oldNscannedObjects;
        int _n; // found so far
        int _oldN;

        int _nYields;
        int _nChunkSkips;

        MatchDetails _details;

        ShardChunkManagerPtr _chunkManager;

        bool _inMemSort;
        auto_ptr< ScanAndOrder > _so;

        shared_ptr<Cursor> _c;
        ClientCursor::CleanupPointer _cc;
        ClientCursor::YieldData _yieldData;

        bool _capped;
        bool _saveClientCursor;
        bool _wouldSaveClientCursor;
        bool _oplogReplay;
        auto_ptr< FindingStartCursor > _findingStartCursor;

        Message &_response;
        ExplainBuilder &_eb;
        CurOp &_curop;
        OpTime _slaveReadTill;

        bool _yieldRecoveryFailed;
    };

    /* run a query -- includes checking for and running a Command \
       @return points to ns if exhaust mode. 0=normal mode
    */
    const char *runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result) {
        shared_ptr<ParsedQuery> pq_shared( new ParsedQuery(q) );
        ParsedQuery& pq( *pq_shared );
        int ntoskip = q.ntoskip;
        BSONObj jsobj = q.query;
        int queryOptions = q.queryOptions;
        const char *ns = q.ns;

        if( logLevel >= 2 )
            log() << "runQuery called " << ns << " " << jsobj << endl;

        curop.debug().ns = ns;
        curop.debug().ntoreturn = pq.getNumToReturn();
        curop.debug().query = jsobj;
        curop.setQuery(jsobj);

        if ( pq.couldBeCommand() ) {
            BufBuilder bb;
            bb.skip(sizeof(QueryResult));
            BSONObjBuilder cmdResBuf;
            if ( runCommands(ns, jsobj, curop, bb, cmdResBuf, false, queryOptions) ) {
                curop.debug().iscommand = true;
                curop.debug().query = jsobj;
                curop.markCommand();

                auto_ptr< QueryResult > qr;
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                curop.debug().responseLength = bb.len();
                qr->setOperation(opReply);
                qr->cursorId = 0;
                qr->startingFrom = 0;
                qr->nReturned = 1;
                result.setData( qr.release(), true );
            }
            else {
                uasserted(13530, "bad or malformed command request?");
            }
            return 0;
        }

        /* --- regular query --- */

        int n = 0;
        BSONObj hint;
        if ( useHints ) {
            hint = pq.getHint();
        }
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

        Client::ReadContext ctx( ns , dbpath ); // read locks
        const ConfigVersion shardingVersionAtStart = shardingState.getVersion( ns );

        replVerifyReadsOk(&pq);

        if ( pq.hasOption( QueryOption_CursorTailable ) ) {
            NamespaceDetails *d = nsdetails( ns );
            uassert( 13051, "tailable cursor requested on non capped collection", d && d->capped );
            const BSONObj nat1 = BSON( "$natural" << 1 );
            if ( order.isEmpty() ) {
                order = nat1;
            }
            else {
                uassert( 13052, "only {$natural:1} order allowed for tailable cursor", order == nat1 );
            }
        }

        if( snapshot ) {
            NamespaceDetails *d = nsdetails(ns);
            if ( d ) {
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
                    hint = BSON( "$hint" << d->idx(i).indexName() );
                }
            }
        }

        if ( ! (explain || pq.showDiskLoc()) && isSimpleIdQuery( query ) && !pq.hasOption( QueryOption_CursorTailable ) ) {

            bool nsFound = false;
            bool indexFound = false;

            BSONObj resObject;
            Client& c = cc();
            bool found = Helpers::findById( c, ns , query , resObject , &nsFound , &indexFound );
            if ( nsFound == false || indexFound == true ) {
                BufBuilder bb(sizeof(QueryResult)+resObject.objsize()+32);
                bb.skip(sizeof(QueryResult));
                
                curop.debug().idhack = true;
                if ( found ) {
                    n = 1;
                    fillQueryResultFromObj( bb , pq.getFields() , resObject );
                }
                auto_ptr< QueryResult > qr;
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                
                curop.debug().responseLength = bb.len();
                qr->setOperation(opReply);
                qr->cursorId = 0;
                qr->startingFrom = 0;
                qr->nReturned = n;
                result.setData( qr.release(), true );
                return NULL;
            }
        }

        // regular, not QO bypass query

        BSONObj oldPlan;
        if ( explain && ! pq.hasIndexSpecifier() ) {
            MultiPlanScanner mps( ns, query, order );
            if ( mps.usingCachedPlan() )
                oldPlan = mps.oldExplain();
        }
        auto_ptr< MultiPlanScanner > mps
        ( new MultiPlanScanner( ns, query, order, hint, !explain, pq.getMin(), pq.getMax(), false,
                               true ) );
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
        
        if ( shardingState.getVersion( ns ) != shardingVersionAtStart ) {
            // if the version changed during the query
            // we might be missing some data
            // and its safe to send this as mongos can resend
            // at this point
            throw SendStaleConfigException( ns , "version changed during initial query" );
        }

        if ( explain ) {
            dqo.finishExplain( explainSuffix );
        }
        n = dqo.n();
        long long nscanned = dqo.totalNscanned();
        curop.debug().scanAndOrder = dqo.scanAndOrderRequired();

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
                shared_ptr< Cursor > multi( new MultiCursor( mps, cursor, dqo.matcher( cursor ), dqo ) );
                cc = new ClientCursor(queryOptions, multi, ns, jsobj.getOwned());
            }
            else {
                if( ! cursor->matcher() ) cursor->setMatcher( dqo.matcher( cursor ) );
                cc = new ClientCursor( queryOptions, cursor, ns, jsobj.getOwned() );
            }

            cc->setChunkManager( dqo.getChunkManager() );

            cursorid = cc->cursorid();
            DEV tlog(2) << "query has more, cursorid: " << cursorid << endl;
            cc->setPos( n );
            cc->pq = pq_shared;
            cc->fields = pq.getFieldPtr();
            cc->originalMessage = m;
            cc->updateLocation();
            if ( !cc->ok() && cc->c()->tailable() )
                DEV tlog() << "query has no more but tailable, cursorid: " << cursorid << endl;
            if( queryOptions & QueryOption_Exhaust ) {
                exhaust = ns;
                curop.debug().exhaust = true;
            }
            dqo.finishForOplogReplay(cc);
        }

        QueryResult *qr = (QueryResult *) result.header();
        qr->cursorId = cursorid;
        curop.debug().cursorid = cursorid == 0 ? -1 : qr->cursorId;
        qr->setResultFlagsToOk();
        // qr->len is updated automatically by appendData()
        curop.debug().responseLength = qr->len;
        qr->setOperation(opReply);
        qr->startingFrom = 0;
        qr->nReturned = n;

        int duration = curop.elapsedMillis();
        bool dbprofile = curop.shouldDBProfile( duration );
        if ( dbprofile || duration >= cmdLine.slowMS ) {
            curop.debug().nscanned = (int) nscanned;
            curop.debug().ntoskip = ntoskip;
        }
        curop.debug().nreturned = n;
        return exhaust;
    }

} // namespace mongo
