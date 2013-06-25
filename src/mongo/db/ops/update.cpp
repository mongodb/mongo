//@file update.cpp

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

#include "mongo/pch.h"

#include "mongo/db/ops/update.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_internal.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/query_runner.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/record.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/server_parameters.h"

//#define DEBUGUPDATE(x) cout << x << endl;
#define DEBUGUPDATE(x)

namespace mongo {

    MONGO_EXPORT_SERVER_PARAMETER( newUpdateFrameworkEnabled, bool, false );

    bool isNewUpdateFrameworkEnabled() {
        return false;
    }

    void checkNoMods( BSONObj o ) {
        BSONObjIterator i( o );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            uassert( 10154 ,  "Modifiers and non-modifiers cannot be mixed", e.fieldName()[ 0 ] != '$' );
        }
    }

    static void checkTooLarge(const BSONObj& newObj) {
        uassert( 12522 , "$ operator made object too large" , newObj.objsize() <= BSONObjMaxUserSize );
    }

    /* note: this is only (as-is) called for

             - not multi
             - not mods is indexed
             - not upsert
    */
    static UpdateResult _updateById(bool isOperatorUpdate,
                                    int idIdxNo,
                                    ModSet* mods,
                                    NamespaceDetails* d,
                                    NamespaceDetailsTransient *nsdt,
                                    bool su,
                                    const char* ns,
                                    const BSONObj& updateobj,
                                    BSONObj patternOrig,
                                    bool logop,
                                    OpDebug& debug,
                                    bool fromMigrate = false) {

        DiskLoc loc;
        {
            IndexDetails& i = d->idx(idIdxNo);
            BSONObj key = i.getKeyFromQuery( patternOrig );
            loc = QueryRunner::fastFindSingle(i, key);
            if( loc.isNull() ) {
                // no upsert support in _updateById yet, so we are done.
                return UpdateResult( 0 , 0 , 0 , BSONObj() );
            }
        }
        Record* r = loc.rec();

        if ( cc().allowedToThrowPageFaultException() && ! r->likelyInPhysicalMemory() ) {
            throw PageFaultException( r );
        }

        /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
           regular ones at the moment. */
        BSONObj newObj;
        if ( isOperatorUpdate ) {
            const BSONObj& onDisk = loc.obj();
            auto_ptr<ModSetState> mss = mods->prepare( onDisk, false /* not an insertion */ );

            if( mss->canApplyInPlace() ) {
                mss->applyModsInPlace(true);
                debug.fastmod = true;
                DEBUGUPDATE( "\t\t\t updateById doing in place update" );

                newObj = onDisk;
            }
            else {
                newObj = mss->createNewFromMods();
                checkTooLarge(newObj);
                verify(nsdt);
                theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , newObj.objdata(), newObj.objsize(), debug);
            }

            if ( logop ) {
                DEV verify( mods->size() );
                BSONObj pattern = patternOrig;
                BSONObj logObj = mss->getOpLogRewrite();
                DEBUGUPDATE( "\t rewrite update: " << logObj );

                // It is possible that the entire mod set was a no-op over this document.  We
                // would have an empty log record in that case. If we call logOp, with an empty
                // record, that would be replicated as "clear this record", which is not what
                // we want. Therefore, to get a no-op in the replica, we simply don't log.
                if ( logObj.nFields() ) {
                    logOp("u", ns, logObj, &pattern, 0, fromMigrate, &newObj );
                }
            }
            return UpdateResult( 1 , 1 , 1 , BSONObj() );

        } // end $operator update

        // regular update
        BSONElementManipulator::lookForTimestamps( updateobj );
        checkNoMods( updateobj );
        verify(nsdt);
        theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , updateobj.objdata(), updateobj.objsize(), debug );
        if ( logop ) {
            logOp("u", ns, updateobj, &patternOrig, 0, fromMigrate, &updateobj );
        }
        return UpdateResult( 1 , 0 , 1 , BSONObj() );
    }

    UpdateResult _updateObjects( bool su,
                                 const char* ns,
                                 const BSONObj& updateobj,
                                 const BSONObj& patternOrig,
                                 bool upsert,
                                 bool multi,
                                 bool logop ,
                                 OpDebug& debug,
                                 RemoveSaver* rs,
                                 bool fromMigrate,
                                 const QueryPlanSelectionPolicy& planPolicy,
                                 bool forReplication ) {

        DEBUGUPDATE( "update: " << ns
                     << " update: " << updateobj
                     << " query: " << patternOrig
                     << " upsert: " << upsert << " multi: " << multi );

        Client& client = cc();

        debug.updateobj = updateobj;

        // The idea with these here it to make them loop invariant for
        // multi updates, and thus be a bit faster for that case.  The
        // pointers may be left invalid on a failed or terminal yield
        // recovery.
        NamespaceDetails* d = nsdetails(ns); // can be null if an upsert...
        NamespaceDetailsTransient* nsdt = &NamespaceDetailsTransient::get(ns);

        auto_ptr<ModSet> mods;
        bool isOperatorUpdate = updateobj.firstElementFieldName()[0] == '$';
        int modsIsIndexed = false; // really the # of indexes
        if ( isOperatorUpdate ) {
            mods.reset( new ModSet(updateobj, nsdt->indexKeys(), forReplication) );
            modsIsIndexed = mods->maxNumIndexUpdated();
        }

        if( planPolicy.permitOptimalIdPlan() && !multi && isSimpleIdQuery(patternOrig) && d &&
           !modsIsIndexed ) {
            int idxNo = d->findIdIndex();
            if( idxNo >= 0 ) {
                debug.idhack = true;

                UpdateResult result = _updateById( isOperatorUpdate,
                                                   idxNo,
                                                   mods.get(),
                                                   d,
                                                   nsdt,
                                                   su,
                                                   ns,
                                                   updateobj,
                                                   patternOrig,
                                                   logop,
                                                   debug,
                                                   fromMigrate);
                if ( result.existing || ! upsert ) {
                    return result;
                }
                else if ( upsert && ! isOperatorUpdate ) {
                    // this handles repl inserts
                    checkNoMods( updateobj );
                    debug.upsert = true;
                    BSONObj no = updateobj;
                    theDataFileMgr.insertWithObjMod(ns, no, false, su);
                    if ( logop )
                        logOp( "i", ns, no, 0, 0, fromMigrate, &no );

                    return UpdateResult( 0 , 0 , 1 , no );
                }
            }
        }

        int numModded = 0;
        debug.nscanned = 0;
        shared_ptr<Cursor> c = getOptimizedCursor( ns, patternOrig, BSONObj(), planPolicy );
        d = nsdetails(ns);
        nsdt = &NamespaceDetailsTransient::get(ns);
        bool autoDedup = c->autoDedup();

        if( c->ok() ) {
            set<DiskLoc> seenObjects;
            MatchDetails details;
            auto_ptr<ClientCursor> cc;
            do {

                if ( cc.get() == 0 &&
                     client.allowedToThrowPageFaultException() &&
                     ! c->currLoc().isNull() &&
                     ! c->currLoc().rec()->likelyInPhysicalMemory() ) {
                    throw PageFaultException( c->currLoc().rec() );
                }

                bool atomic = c->matcher() && c->matcher()->docMatcher().atomic();

                if ( ! atomic && debug.nscanned > 0 ) {
                    // we need to use a ClientCursor to yield
                    if ( cc.get() == 0 ) {
                        shared_ptr< Cursor > cPtr = c;
                        cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , cPtr , ns ) );
                    }

                    bool didYield;
                    if ( ! cc->yieldSometimes( ClientCursor::WillNeed, &didYield ) ) {
                        cc.release();
                        break;
                    }
                    if ( !c->ok() ) {
                        break;
                    }

                    if ( didYield ) {
                        d = nsdetails(ns);
                        if ( ! d )
                            break;
                        nsdt = &NamespaceDetailsTransient::get(ns);
                        if ( mods.get() ) {
                            mods->setIndexedStatus( nsdt->indexKeys() );
                            modsIsIndexed = mods->maxNumIndexUpdated();
                        }

                    }

                } // end yielding block

                debug.nscanned++;

                if ( mods.get() && mods->hasDynamicArray() ) {
                    details.requestElemMatchKey();
                }

                if ( !c->currentMatches( &details ) ) {
                    c->advance();
                    continue;
                }

                Record* r = c->_current();
                DiskLoc loc = c->currLoc();

                if ( c->getsetdup( loc ) && autoDedup ) {
                    c->advance();
                    continue;
                }

                BSONObj js = BSONObj::make(r);

                BSONObj pattern = patternOrig;

                if ( logop ) {
                    BSONObjBuilder idPattern;
                    BSONElement id;
                    // NOTE: If the matching object lacks an id, we'll log
                    // with the original pattern.  This isn't replay-safe.
                    // It might make sense to suppress the log instead
                    // if there's no id.
                    if ( js.getObjectID( id ) ) {
                        idPattern.append( id );
                        pattern = idPattern.obj();
                    }
                    else {
                        uassert( 10157 ,  "multi-update requires all modified objects to have an _id" , ! multi );
                    }
                }

                /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
                    regular ones at the moment. */
                if ( isOperatorUpdate ) {

                    if ( multi ) {
                        // go to next record in case this one moves
                        c->advance();

                        // Update operations are deduped for cursors that implement their own
                        // deduplication.  In particular, some geo cursors are excluded.
                        if ( autoDedup ) {

                            if ( seenObjects.count( loc ) ) {
                                continue;
                            }

                            // SERVER-5198 Advance past the document to be modified, provided
                            // deduplication is enabled, but see SERVER-5725.
                            while( c->ok() && loc == c->currLoc() ) {
                                c->advance();
                            }
                        }
                    }

                    const BSONObj& onDisk = loc.obj();

                    ModSet* useMods = mods.get();

                    auto_ptr<ModSet> mymodset;
                    if ( details.hasElemMatchKey() && mods->hasDynamicArray() ) {
                        useMods = mods->fixDynamicArray( details.elemMatchKey() );
                        mymodset.reset( useMods );
                    }

                    auto_ptr<ModSetState> mss = useMods->prepare( onDisk,
                                                                  false /* not an insertion */ );

                    bool willAdvanceCursor = multi && c->ok() && ( modsIsIndexed || ! mss->canApplyInPlace() );

                    if ( willAdvanceCursor ) {
                        if ( cc.get() ) {
                            cc->setDoingDeletes( true );
                        }
                        c->prepareToTouchEarlierIterate();
                    }

                    // If we've made it this far, "ns" must contain a valid collection name, and so
                    // is of the form "db.collection".  Therefore, the following expression must
                    // always be valid.  "system.users" updates must never be done in place, in
                    // order to ensure that they are validated inside DataFileMgr::updateRecord(.).
                    bool isSystemUsersMod = nsToCollectionSubstring(ns) == "system.users";

                    BSONObj newObj;
                    if ( !mss->isUpdateIndexed() && mss->canApplyInPlace() && !isSystemUsersMod ) {
                        mss->applyModsInPlace( true );// const_cast<BSONObj&>(onDisk) );

                        DEBUGUPDATE( "\t\t\t doing in place update" );
                        if ( !multi )
                            debug.fastmod = true;

                        if ( modsIsIndexed ) {
                            seenObjects.insert( loc );
                        }
                        newObj = loc.obj();
                        d->paddingFits();
                    }
                    else {
                        newObj = mss->createNewFromMods();
                        checkTooLarge(newObj);
                        DiskLoc newLoc = theDataFileMgr.updateRecord(ns,
                                                                     d,
                                                                     nsdt,
                                                                     r,
                                                                     loc,
                                                                     newObj.objdata(),
                                                                     newObj.objsize(),
                                                                     debug);

                        if ( newLoc != loc || modsIsIndexed ){
                            // log() << "Moved obj " << newLoc.obj()["_id"] << " from " << loc << " to " << newLoc << endl;
                            // object moved, need to make sure we don' get again
                            seenObjects.insert( newLoc );
                        }

                    }

                    if ( logop ) {
                        DEV verify( mods->size() );
                        BSONObj logObj = mss->getOpLogRewrite();
                        DEBUGUPDATE( "\t rewrite update: " << logObj );

                        // It is possible that the entire mod set was a no-op over this
                        // document.  We would have an empty log record in that case. If we
                        // call logOp, with an empty record, that would be replicated as "clear
                        // this record", which is not what we want. Therefore, to get a no-op
                        // in the replica, we simply don't log.
                        if ( logObj.nFields() ) {
                            logOp("u", ns, logObj , &pattern, 0, fromMigrate, &newObj );
                        }
                    }
                    numModded++;
                    if ( ! multi )
                        return UpdateResult( 1 , 1 , numModded , BSONObj() );
                    if ( willAdvanceCursor )
                        c->recoverFromTouchingEarlierIterate();

                    getDur().commitIfNeeded();

                    continue;
                }

                uassert( 10158 ,  "multi update only works with $ operators" , ! multi );

                BSONElementManipulator::lookForTimestamps( updateobj );
                checkNoMods( updateobj );
                theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , updateobj.objdata(), updateobj.objsize(), debug, su);
                if ( logop ) {
                    DEV wassert( !su ); // super used doesn't get logged, this would be bad.
                    logOp("u", ns, updateobj, &pattern, 0, fromMigrate, &updateobj );
                }
                return UpdateResult( 1 , 0 , 1 , BSONObj() );
            } while ( c->ok() );
        } // endif

        if ( numModded )
            return UpdateResult( 1 , 1 , numModded , BSONObj() );

        if ( upsert ) {
            if ( updateobj.firstElementFieldName()[0] == '$' ) {
                // upsert of an $operation. build a default object
                BSONObj newObj = mods->createNewFromQuery( patternOrig );
                checkNoMods( newObj );
                debug.fastmodinsert = true;
                theDataFileMgr.insertWithObjMod(ns, newObj, false, su);
                if ( logop )
                    logOp( "i", ns, newObj, 0, 0, fromMigrate, &newObj );

                return UpdateResult( 0 , 1 , 1 , newObj );
            }
            uassert( 10159 ,  "multi update only works with $ operators" , ! multi );
            checkNoMods( updateobj );
            debug.upsert = true;
            BSONObj no = updateobj;
            theDataFileMgr.insertWithObjMod(ns, no, false, su);
            if ( logop )
                logOp( "i", ns, no, 0, 0, fromMigrate, &no );
            return UpdateResult( 0 , 0 , 1 , no );
        }

        return UpdateResult( 0 , isOperatorUpdate , 0 , BSONObj() );
    }

    void validateUpdate( const char* ns , const BSONObj& updateobj, const BSONObj& patternOrig ) {
        uassert( 10155 , "cannot update reserved $ collection", strchr(ns, '$') == 0 );
        if ( strstr(ns, ".system.") ) {
            /* dm: it's very important that system.indexes is never updated as IndexDetails
               has pointers into it */
            uassert( 10156,
                     str::stream() << "cannot update system collection: "
                                   << ns << " q: " << patternOrig << " u: " << updateobj,
                     legalClientSystemNS( ns , true ) );
        }
    }

    UpdateResult _updateObjectsNEW( bool su,
                                    const char* ns,
                                    const BSONObj& updateobj,
                                    const BSONObj& patternOrig,
                                    bool upsert,
                                    bool multi,
                                    bool logop ,
                                    OpDebug& debug,
                                    RemoveSaver* rs,
                                    bool fromMigrate,
                                    const QueryPlanSelectionPolicy& planPolicy,
                                    bool forReplication ) {

        // TODO
        // + Separate UpdateParser from UpdateRunner (the latter should be "stage-y")
        // + fast path for update for query by _id
        // + $atomic support (or better, support proper yielding if not)
        // + define the dedup story (and do it here, if that's the decidion)
        // + support in-place updates (and determination if indices are involved in an update)
        // + update OpDebug counters properly
        // + specific paths set on insert
        // + support for relaxing viable path constraint in replication
        // + set UpdateResponse properly

        UpdateDriver::Options opts;
        opts.multi = multi;
        opts.upsert = upsert;
        opts.logOp = logop;
        UpdateDriver driver(opts);
        Status status = driver.parse( updateobj );
        if ( !status.isOK() ) {
            uasserted( 16840, status.reason() );
        }

        shared_ptr<Cursor> cursor = getOptimizedCursor( ns, patternOrig, BSONObj(), planPolicy );
        NamespaceDetails* d = nsdetails(ns); // can be null if an upsert...
        NamespaceDetailsTransient* nsdt = &NamespaceDetailsTransient::get(ns);

        // We may or may not have documents for this update. If we don't, then try to upsert,
        // if allowed.
        if ( !cursor->ok() && upsert ) {

            // If this is not a full object replace, we need to generate a document by
            // examining the query. Otherwise, we can use the replacement object itself.
            BSONObj oldObj;
            if ( *updateobj.firstElementFieldName() == '$' ) {
                if ( !driver.createFromQuery( patternOrig, &oldObj ) ) {
                    uasserted( 16835, "cannot create object to update" );
                }
                // TODO this is the hook for activating a $setOnInsert
            }

            // Since this is an upsert, we will be oplogging it as an insert. We don't
            // need the driver's help to build the oplog record, then.
            driver.setLogOp(false);

            BSONObj newObj;
            status = driver.update( oldObj, StringData(), &newObj, NULL /* no oplog record */);
            if ( !status.isOK() ) {
                uasserted( 16836, status.reason() );
            }

            theDataFileMgr.insertWithObjMod( ns, newObj, false, su );

            if ( logop ) {
                logOp( "i", ns, newObj, 0, 0, fromMigrate, &newObj );
            }

            return UpdateResult( false, false, 0, BSONObj() );
        }

        // We have documents for this update. Let's fetch each of them and pipe them through
        // the update expression.
        while ( cursor->ok() ) {

            // Get Obj and match details
            Record* r = cursor->_current();
            DiskLoc loc = cursor->currLoc();
            const BSONObj oldObj = loc.obj();


            // Skip documents that don't match. Also, we don't want to update the very object
            // atop of which the cursor is, becuase that document may move. So we advance the
            // cursor before operating on the record.
            MatchDetails matchDetails;
            matchDetails.requestElemMatchKey();
            if ( !cursor->currentMatches( &matchDetails ) ) {
                cursor->advance();
                continue;
            }
            else if (multi) {
                cursor->advance();
            }

            BSONObj newObj;
            BSONObj logObj;
            status = driver.update( oldObj, matchDetails.elemMatchKey(), &newObj, &logObj );
            if ( !status.isOK() ) {
                uasserted( 16837, status.reason() );
            }

            // Write Obj
            theDataFileMgr.updateRecord(ns,
                                        d,
                                        nsdt,
                                        r,
                                        loc,
                                        newObj.objdata(),
                                        newObj.objsize(),
                                        debug);

            // Log Obj
            if ( logop ) {
                if ( !logObj.isEmpty() ) {
                    BSONObj pattern = patternOrig;
                    logOp("u", ns, logObj , &pattern, 0, fromMigrate, &newObj );
                }
            }

            getDur().commitIfNeeded();

            if (!multi) {
                break;
            }
        }

        return UpdateResult( false, false, 0, BSONObj() );
    }

    UpdateResult updateObjects( const char* ns,
                                const BSONObj& updateobj,
                                const BSONObj& patternOrig,
                                bool upsert,
                                bool multi,
                                bool logop ,
                                OpDebug& debug,
                                bool fromMigrate,
                                const QueryPlanSelectionPolicy& planPolicy ) {

        validateUpdate( ns , updateobj , patternOrig );

        if ( isNewUpdateFrameworkEnabled() ) {

            UpdateResult ur = _updateObjectsNEW(false, ns, updateobj, patternOrig,
                                                upsert, multi, logop,
                                                debug, NULL, fromMigrate, planPolicy );
            debug.nupdated = ur.num;
            return ur;
        }
        else {

            UpdateResult ur = _updateObjects(false, ns, updateobj, patternOrig,
                                             upsert, multi, logop,
                                             debug, NULL, fromMigrate, planPolicy );
            debug.nupdated = ur.num;
            return ur;
        }
    }

    UpdateResult updateObjectsForReplication( const char* ns,
                                              const BSONObj& updateobj,
                                              const BSONObj& patternOrig,
                                              bool upsert,
                                              bool multi,
                                              bool logop ,
                                              OpDebug& debug,
                                              bool fromMigrate,
                                              const QueryPlanSelectionPolicy& planPolicy ) {

        validateUpdate( ns , updateobj , patternOrig );

        if ( isNewUpdateFrameworkEnabled() ) {

            UpdateResult ur = _updateObjectsNEW(false,
                                                ns,
                                                updateobj,
                                                patternOrig,
                                                upsert,
                                                multi,
                                                logop,
                                                debug,
                                                NULL /* no remove saver */,
                                                fromMigrate,
                                                planPolicy,
                                                true /* for replication */ );
            debug.nupdated = ur.num;
            return ur;

        }
        else {

            UpdateResult ur = _updateObjects(false,
                                             ns,
                                             updateobj,
                                             patternOrig,
                                             upsert,
                                             multi,
                                             logop,
                                             debug,
                                             NULL /* no remove saver */,
                                             fromMigrate,
                                             planPolicy,
                                             true /* for replication */ );
            debug.nupdated = ur.num;
            return ur;

        }
    }

    BSONObj applyUpdateOperators( const BSONObj& from, const BSONObj& operators ) {
        if ( isNewUpdateFrameworkEnabled() ) {
            UpdateDriver::Options opts;
            opts.multi = false;
            opts.upsert = false;
            UpdateDriver driver(opts);
            Status status = driver.parse( operators );
            if ( !status.isOK() ) {
                uasserted( 16838, status.reason() );
            }

            BSONObj newObj;
            status = driver.update( from, StringData(), &newObj, NULL /* not oplogging */ );
            if ( !status.isOK() ) {
                uasserted( 16839, status.reason() );
            }

            return newObj;
        }
        else {
            ModSet mods( operators );
            return mods.prepare( from, false /* not an insertion */ )->createNewFromMods();
        }
    }

}  // namespace mongo
