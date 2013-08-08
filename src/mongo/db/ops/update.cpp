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

#include "mongo/pch.h"

#include "mongo/db/ops/update.h"

#include <cstring>  // for memcpy

#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/index_set.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/query_runner.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    namespace {

        // TODO: Make this a function on NamespaceString, or make it cleaner.
        inline void validateUpdate( const char* ns , const BSONObj& updateobj, const BSONObj& patternOrig ) {
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

        /**
         * return a BSONObj with the _id field of the doc passed in. If no _id and multi, error.
         */
        BSONObj makeOplogEntryQuery(const BSONObj doc, bool multi) {
            BSONObjBuilder idPattern;
            BSONElement id;
            // NOTE: If the matching object lacks an id, we'll log
            // with the original pattern.  This isn't replay-safe.
            // It might make sense to suppress the log instead
            // if there's no id.
            if ( doc.getObjectID( id ) ) {
                idPattern.append( id );
                return idPattern.obj();
            }
            else {
                uassert( 10157, "multi-update requires all modified objects to have an _id" , ! multi );
                return doc;
            }
        }

    } // namespace

    UpdateResult update(const UpdateRequest& request, OpDebug* opDebug) {

        // Should the modifiers validate their embedded docs via okForStorage
        // Only user updates should be checked. Any system or replication stuff should pass through.
        // Config db docs shouldn't get checked for valid field names since the shard key can have
        // a dot (".") in it.
        bool shouldValidate = !(request.isFromReplication() ||
                                request.getNamespaceString().isConfigDB());

        // TODO: Consider some sort of unification between the UpdateDriver, ModifierInterface
        // and UpdateRequest structures.
        UpdateDriver::Options opts;
        opts.multi = request.isMulti();
        opts.upsert = request.isUpsert();
        opts.logOp = request.shouldUpdateOpLog();
        opts.modOptions = ModifierInterface::Options( request.isFromReplication(), shouldValidate );
        UpdateDriver driver( opts );

        Status status = driver.parse( request.getUpdates() );
        if ( !status.isOK() ) {
            uasserted( 16840, status.reason() );
        }

        return update(request, opDebug, &driver);
    }

    UpdateResult update(const UpdateRequest& request, OpDebug* opDebug, UpdateDriver* driver) {

        const NamespaceString& nsString = request.getNamespaceString();

        validateUpdate( nsString.ns().c_str(), request.getUpdates(), request.getQuery() );

        NamespaceDetails* nsDetails = nsdetails( nsString.ns() );
        NamespaceDetailsTransient* nsDetailsTransient =
            &NamespaceDetailsTransient::get( nsString.ns().c_str() );

        // TODO: This seems a bit circuitious.
        opDebug->updateobj = request.getUpdates();

        driver->refreshIndexKeys( nsDetailsTransient->indexKeys() );

        shared_ptr<Cursor> cursor = getOptimizedCursor(
            nsString.ns(), request.getQuery(), BSONObj(), request.getQueryPlanSelectionPolicy() );

        // If the update was marked with '$isolated' (a.k.a '$atomic'), we are not allowed to
        // yield while evaluating the update loop below.
        //
        // TODO: Old code checks this repeatedly within the update loop. Is that necessary? It seems
        // that once atomic should be always atomic.
        const bool isolated =
            cursor->ok() &&
            cursor->matcher() &&
            cursor->matcher()->docMatcher().atomic();

        // The 'cursor' the optimizer gave us may contain query plans that generate duplicate
        // diskloc's. We set up here the mechanims that will prevent us from processing those
        // twice if we see them. We also set up a 'ClientCursor' so that we can support
        // yielding.
        //
        // TODO: Is it valid to call this on a non-ok cursor?
        const bool dedupHere = cursor->autoDedup();

        //
        // We'll start assuming we have one or more documents for this update. (Othwerwise,
        // we'll fallback to upserting.)
        //

        // We record that this will not be an upsert, in case a mod doesn't want to be applied
        // when in strict update mode.
        driver->setContext( ModifierInterface::ExecInfo::UPDATE_CONTEXT );

        // Let's fetch each of them and pipe them through the update expression, making sure to
        // keep track of the necessary stats. Recall that we'll be pulling documents out of
        // cursors and some of them do not deduplicate the entries they generate. We have
        // deduping logic in here, too -- for now.
        unordered_set<DiskLoc, DiskLoc::Hasher> seenLocs;
        int numMatched = 0;
        opDebug->nscanned = 0;

        Client& client = cc();

        mutablebson::Document doc;
        mutablebson::DamageVector damages;

        // If we are going to be yielding, we will need a ClientCursor scoped to this loop. We
        // only loop as long as the underlying cursor is OK.
        for ( auto_ptr<ClientCursor> clientCursor; cursor->ok(); ) {

            // If we haven't constructed a ClientCursor, and if the client allows us to throw
            // page faults, and if we are referring to a location that is likely not in
            // physical memory, then throw a PageFaultException. The entire operation will be
            // restarted.
            if ( clientCursor.get() == NULL &&
                 client.allowedToThrowPageFaultException() &&
                 !cursor->currLoc().isNull() &&
                 !cursor->currLoc().rec()->likelyInPhysicalMemory() ) {
                // We should never throw a PFE if we have already updated items.
                dassert((numMatched == 0) || (numMatched == opDebug->nupdateNoops));
                throw PageFaultException( cursor->currLoc().rec() );
            }

            if ( !isolated && opDebug->nscanned != 0 ) {

                // We are permitted to yield. To do so we need a ClientCursor, so create one
                // now if we have not yet done so.
                if ( !clientCursor.get() )
                    clientCursor.reset(
                        new ClientCursor( QueryOption_NoCursorTimeout, cursor, nsString.ns() ) );

                // Ask the client cursor to yield. We get two bits of state back: whether or not
                // we yielded, and whether or not we correctly recovered from yielding.
                bool yielded = false;
                const bool recovered = clientCursor->yieldSometimes(
                    ClientCursor::WillNeed, &yielded );

                if ( !recovered ) {
                    // If we failed to recover from the yield, then the ClientCursor is already
                    // gone. Release it so we don't destroy it a second time.
                    clientCursor.release();
                    break;
                }

                if ( !cursor->ok() ) {
                    // If the cursor died while we were yielded, just get out of the update loop.
                    break;
                }

                if ( yielded ) {
                    // We yielded and recovered OK, and our cursor is still good. Details about
                    // our namespace may have changed while we were yielded, so we re-acquire
                    // them here. If we can't do so, escape the update loop. Otherwise, refresh
                    // the driver so that it knows about what is currently indexed.
                    nsDetails = nsdetails( nsString.ns() );
                    if ( !nsDetails )
                        break;
                    nsDetailsTransient = &NamespaceDetailsTransient::get( nsString.ns().c_str() );

                    // TODO: This copies the index keys, but it may not need to do so.
                    driver->refreshIndexKeys( nsDetailsTransient->indexKeys() );
                }

            }

            // Let's fetch the next candidate object for this update.
            Record* record = cursor->_current();
            DiskLoc loc = cursor->currLoc();
            const BSONObj oldObj = loc.obj();

            // We count how many documents we scanned even though we may skip those that are
            // deemed duplicated. The final 'numUpdated' and 'nscanned' numbers may differ for
            // that reason.
            opDebug->nscanned++;

            // Skips this document if it:
            // a) doesn't match the query portion of the update
            // b) was deemed duplicate by the underlying cursor machinery
            //
            // Now, if we are going to update the document,
            // c) we don't want to do so while the cursor is at it, as that may invalidate
            // the cursor. So, we advance to next document, before issuing the update.
            MatchDetails matchDetails;
            matchDetails.requestElemMatchKey();
            if ( !cursor->currentMatches( &matchDetails ) ) {
                // a)
                cursor->advance();
                continue;
            }
            else if ( cursor->getsetdup( loc ) && dedupHere ) {
                // b)
                cursor->advance();
                continue;
            }
            else if (!driver->isDocReplacement() && request.isMulti()) {
                // c)
                cursor->advance();
                if ( dedupHere ) {
                    if ( seenLocs.count( loc ) ) {
                        continue;
                    }
                }

                // There are certain kind of cursors that hold multiple pointers to data
                // underneath. $or cursors is one example. In a $or cursor, it may be the case
                // that when we did the last advance(), we finished consuming documents from
                // one of $or child and started consuming the next one. In that case, it is
                // possible that the last document of the previous child is the same as the
                // first document of the next (see SERVER-5198 and jstests/orp.js).
                //
                // So we advance the cursor here until we see a new diskloc.
                //
                // Note that we won't be yielding, and we may not do so for a while if we find
                // a particularly duplicated sequence of loc's. That is highly unlikely,
                // though.  (See SERVER-5725, if curious, but "stage" based $or will make that
                // ticket moot).
                while( cursor->ok() && loc == cursor->currLoc() ) {
                    cursor->advance();
                }
            }

            // For some (unfortunate) historical reasons, not all cursors would be valid after
            // a write simply because we advanced them to a document not affected by the write.
            // To protect in those cases, not only we engaged in the advance() logic above, but
            // we also tell the cursor we're about to write a document that we've just seen.
            // prepareToTouchEarlierIterate() requires calling later
            // recoverFromTouchingEarlierIterate(), so we make a note here to do so.
            bool touchPreviousDoc = request.isMulti() && cursor->ok();
            if ( touchPreviousDoc ) {
                if ( clientCursor.get() )
                    clientCursor->setDoingDeletes( true );
                cursor->prepareToTouchEarlierIterate();
            }

            // Found a matching document
            numMatched++;

            // Ask the driver to apply the mods. It may be that the driver can apply those "in
            // place", that is, some values of the old document just get adjusted without any
            // change to the binary layout on the bson layer. It may be that a whole new
            // document is needed to accomodate the new bson layout of the resulting document.
            doc.reset( oldObj, mutablebson::Document::kInPlaceEnabled );
            BSONObj logObj;

            // If there was a matched field, obtain it.
            string matchedField;
            if (matchDetails.hasElemMatchKey())
                matchedField = matchDetails.elemMatchKey();

            Status status = driver->update( matchedField, &doc, &logObj );
            if ( !status.isOK() ) {
                uasserted( 16837, status.reason() );
            }

            // If the driver applied the mods in place, we can ask the mutable for what
            // changed. We call those changes "damages". :) We use the damages to inform the
            // journal what was changed, and then apply them to the original document
            // ourselves. If, however, the driver applied the mods out of place, we ask it to
            // generate a new, modified document for us. In that case, the file manager will
            // take care of the journaling details for us.
            //
            // This code flow is admittedly odd. But, right now, journaling is baked in the file
            // manager. And if we aren't using the file manager, we have to do jounaling
            // ourselves.
            bool objectWasChanged = false;
            BSONObj newObj;
            const char* source = NULL;
            bool inPlace = doc.getInPlaceUpdates(&damages, &source);
            if ( inPlace && !damages.empty() && !driver->modsAffectIndices() ) {
                nsDetails->paddingFits();

                // All updates were in place. Apply them via durability and writing pointer.
                mutablebson::DamageVector::const_iterator where = damages.begin();
                const mutablebson::DamageVector::const_iterator end = damages.end();
                for( ; where != end; ++where ) {
                    const char* sourcePtr = source + where->sourceOffset;
                    void* targetPtr = getDur().writingPtr(
                        const_cast<char*>(oldObj.objdata()) + where->targetOffset,
                        where->size);
                    std::memcpy(targetPtr, sourcePtr, where->size);
                }
                newObj = oldObj;
                opDebug->fastmod = true;

                objectWasChanged = true;
            }
            else {

                // The updates were not in place. Apply them through the file manager.
                newObj = doc.getObject();
                DiskLoc newLoc = theDataFileMgr.updateRecord(nsString.ns().c_str(),
                                                             nsDetails,
                                                             nsDetailsTransient,
                                                             record,
                                                             loc,
                                                             newObj.objdata(),
                                                             newObj.objsize(),
                                                             *opDebug);

                // If we've moved this object to a new location, make sure we don't apply
                // that update again if our traversal picks the objecta again.
                //
                // We also take note that the diskloc if the updates are affecting indices.
                // Chances are that we're traversing one of them and they may be multi key and
                // therefore duplicate disklocs.
                if ( newLoc != loc || driver->modsAffectIndices()  ) {
                    seenLocs.insert( newLoc );
                }

                objectWasChanged = true;
            }

            // Log Obj
            if ( request.shouldUpdateOpLog() ) {
                if ( driver->isDocReplacement() || !logObj.isEmpty() ) {
                    BSONObj idQuery = driver->makeOplogEntryQuery(newObj, request.isMulti());
                    logOp("u", nsString.ns().c_str(), logObj , &idQuery,
                          NULL, request.isFromMigration(), &newObj);
                }
            }

            // If it was noop since the document didn't change, record that.
            if (!objectWasChanged)
                opDebug->nupdateNoops++;

            if (!request.isMulti()) {
                break;
            }

            // If we used the cursor mechanism that prepares an earlier seen document for a
            // write we need to tell such mechanisms that the write is over.
            if ( touchPreviousDoc ) {
                cursor->recoverFromTouchingEarlierIterate();
            }

            getDur().commitIfNeeded();

        }

        // TODO: Can this be simplified?
        if ((numMatched > 0) || (numMatched == 0 && !request.isUpsert()) ) {
            opDebug->nupdated = numMatched;
            return UpdateResult( numMatched > 0 /* updated existing object(s) */,
                                 !driver->isDocReplacement() /* $mod or obj replacement */,
                                 numMatched /* # of docments update, even no-ops */,
                                 BSONObj() );
        }

        //
        // We haven't found any existing document so an insert is done
        // (upsert is true).
        //
        opDebug->upsert = true;

        // Since this is an insert (no docs found and upsert:true), we will be logging it
        // as an insert in the oplog. We don't need the driver's help to build the
        // oplog record, then. We also set the context of the update driver to the INSERT_CONTEXT.
        // Some mods may only work in that context (e.g. $setOnInsert).
        driver->setLogOp( false );
        driver->setContext( ModifierInterface::ExecInfo::INSERT_CONTEXT );

        BSONObj baseObj;

        // Reset the document we will be writing to
        doc.reset( baseObj, mutablebson::Document::kInPlaceDisabled );
        if ( request.getQuery().hasElement("_id") ) {
            uassertStatusOK(doc.root().appendElement(request.getQuery().getField("_id")));
        }


        // If this is a $mod base update, we need to generate a document by examining the
        // query and the mods. Otherwise, we can use the object replacement sent by the user
        // update command that was parsed by the driver before.
        // In the following block we handle the query part, and then do the regular mods after.
        if ( *request.getUpdates().firstElementFieldName() == '$' ) {
            uassertStatusOK(UpdateDriver::createFromQuery(request.getQuery(), doc));
            opDebug->fastmodinsert = true;
        }

        // Apply the update modifications and then log the update as an insert manually.
        Status status = driver->update( StringData(), &doc, NULL /* no oplog record */);
        if ( !status.isOK() ) {
            uasserted( 16836, status.reason() );
        }

        BSONObj newObj = doc.getObject();
        theDataFileMgr.insertWithObjMod( nsString.ns().c_str(), newObj, false, request.isGod() );
        if ( request.shouldUpdateOpLog() ) {
            logOp( "i", nsString.ns().c_str(), newObj,
                   NULL, NULL, request.isFromMigration(), &newObj );
        }

        opDebug->nupdated = 1;
        return UpdateResult( false /* updated a non existing document */,
                             !driver->isDocReplacement() /* $mod or obj replacement? */,
                             1 /* count of updated documents */,
                             newObj /* object that was upserted */ );
    }

    BSONObj applyUpdateOperators( const BSONObj& from, const BSONObj& operators ) {
        UpdateDriver::Options opts;
        opts.multi = false;
        opts.upsert = false;
        UpdateDriver driver( opts );
        Status status = driver.parse( operators );
        if ( !status.isOK() ) {
            uasserted( 16838, status.reason() );
        }

        mutablebson::Document doc( from, mutablebson::Document::kInPlaceDisabled );
        status = driver.update( StringData(), &doc, NULL /* not oplogging */ );
        if ( !status.isOK() ) {
            uasserted( 16839, status.reason() );
        }

        return doc.getObject();
    }

}  // namespace mongo
