// @file explain.cpp - Helper classes for generating query explain output.

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/db/explain.h"

#include "mongo/db/cmdline.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
    
    // !!! TODO get rid of const casts

    ExplainPlanInfo::ExplainPlanInfo() :
    _isMultiKey(),
    _n(),
    _nscannedObjects(),
    _nscanned(),
    _scanAndOrder(),
    _indexOnly(),
    _picked(),
    _done() {
    }

    void ExplainPlanInfo::notePlan( const Cursor &cursor, bool scanAndOrder, bool indexOnly ) {
        _cursorName = const_cast<Cursor&>(cursor).toString();
        _indexBounds = cursor.prettyIndexBounds().getOwned();
        _scanAndOrder = scanAndOrder;
        _indexOnly = indexOnly;
        noteCursorUpdate( cursor );
    }
    
    void ExplainPlanInfo::noteIterate( bool match, bool loadedRecord, const Cursor &cursor ) {
        if ( match ) {
            ++_n;
        }
        if ( loadedRecord ) {
            ++_nscannedObjects;
        }
        noteCursorUpdate( cursor );
    }
    
    void ExplainPlanInfo::noteDone( const Cursor &cursor ) {
        _done = true;
        noteCursorUpdate( cursor );
        BSONObjBuilder bob;
        const_cast<Cursor&>(cursor).explainDetails( bob );
        _details = bob.obj();
    }
    
    void ExplainPlanInfo::notePicked() {
        _picked = true;
    }

    BSONObj ExplainPlanInfo::bson() const {
        BSONObjBuilder bob;
        bob.append( "cursor", _cursorName );
        bob.appendNumber( "n", _n );
        bob.appendNumber( "nscannedObjects", _nscannedObjects );
        bob.appendNumber( "nscanned", _nscanned );
        bob.append( "indexBounds", _indexBounds );
        return bob.obj();
    }
    
    BSONObj ExplainPlanInfo::pickedPlanBson( const ExplainClauseInfo &clauseInfo ) const {
        BSONObjBuilder bob;
        bob.append( "cursor", _cursorName );
        bob.append( "isMultiKey", _isMultiKey );
        bob.appendNumber( "n", clauseInfo.n() );
        bob.appendNumber( "nscannedObjects", clauseInfo.nscannedObjects() );
        bob.appendNumber( "nscanned", clauseInfo.nscanned() );
        bob.appendNumber( "nscannedObjectsAllPlans", clauseInfo.nscannedObjectsAllPlans() );
        bob.appendNumber( "nscannedAllPlans", clauseInfo.nscannedAllPlans() );
        bob.append( "scanAndOrder", _scanAndOrder );
        bob.append( "indexOnly", _indexOnly );
        bob.appendNumber( "nYields", clauseInfo.nYields() );
        bob.appendNumber( "nChunkSkips", clauseInfo.nChunkSkips() );
        bob.appendNumber( "millis", clauseInfo.millis() );
        bob.append( "indexBounds", _indexBounds );
        bob.appendElements( _details );
        return bob.obj();
    }

    void ExplainPlanInfo::noteCursorUpdate( const Cursor &cursor ) {
        _isMultiKey = cursor.isMultiKey();
        _nscanned = const_cast<Cursor&>(cursor).nscanned();
    }

    ExplainClauseInfo::ExplainClauseInfo() :
    _n(),
    _nscannedObjects(),
    _nChunkSkips(),
    _nYields() {
    }
    
    BSONObj ExplainClauseInfo::bson() const {
        BSONObjBuilder bb;
        bb.appendElements( virtualPickedPlan().pickedPlanBson( *this ) );
        BSONArrayBuilder allPlans( bb.subarrayStart( "allPlans" ) );
        for( list<shared_ptr<const ExplainPlanInfo> >::const_iterator i = _plans.begin();
            i != _plans.end(); ++i ) {
            allPlans << (*i)->bson();
        }
        allPlans.done();
        return bb.obj();
    }

    void ExplainClauseInfo::addPlanInfo( const shared_ptr<ExplainPlanInfo> &info ) {
        _plans.push_back( info );
    }
    
    void ExplainClauseInfo::noteYield() { ++_nYields; }
    
    void ExplainClauseInfo::noteIterate( bool match, bool loadedRecord, bool chunkSkip ) {
        if ( match ) {
            ++_n;
        }
        if ( loadedRecord ) {
            ++_nscannedObjects;
        }
        if ( chunkSkip ) {
            ++_nChunkSkips;
        }
    }

    void ExplainClauseInfo::reviseN( long long n ) {
        _n = n;
    }

    void ExplainClauseInfo::stopTimer() {
        _timer.stop();
    }

    long long ExplainClauseInfo::nscannedObjects() const {
        if ( _plans.empty() ) {
            return 0;
        }
        return virtualPickedPlan().nscannedObjects();
    }

    long long ExplainClauseInfo::nscanned() const {
        if ( _plans.empty() ) {
            return 0;
        }
        return virtualPickedPlan().nscanned();
    }

    long long ExplainClauseInfo::nscannedAllPlans() const {
        long long ret = 0;
        for( list<shared_ptr<const ExplainPlanInfo> >::const_iterator i = _plans.begin();
            i != _plans.end(); ++i ) {
            ret += (*i)->nscanned();
        }
        return ret;
    }

    const ExplainPlanInfo &ExplainClauseInfo::virtualPickedPlan() const {
        // Return a picked plan if possible.
        for( list<shared_ptr<const ExplainPlanInfo> >::const_iterator i = _plans.begin();
            i != _plans.end(); ++i ) {
            if ( (*i)->picked() ) {
                return **i;
            }
        }
        // Return a done plan if possible.
        for( list<shared_ptr<const ExplainPlanInfo> >::const_iterator i = _plans.begin();
            i != _plans.end(); ++i ) {
            if ( (*i)->done() ) {
                return **i;
            }
        }
        // Return a plan with the highest match count.
        long long maxN = -1;
        shared_ptr<const ExplainPlanInfo> ret;
        for( list<shared_ptr<const ExplainPlanInfo> >::const_iterator i = _plans.begin();
            i != _plans.end(); ++i ) {
            long long n = ( *i )->n();
            if ( n > maxN ) {
                maxN = n;
                ret = *i;
            }
        }
        verify( ret );
        return *ret;
    }
    
    void ExplainQueryInfo::noteIterate( bool match, bool loadedRecord, bool chunkSkip ) {
        verify( !_clauses.empty() );
        _clauses.back()->noteIterate( match, loadedRecord, chunkSkip );
    }

    void ExplainQueryInfo::noteYield() {
        verify( !_clauses.empty() );
        _clauses.back()->noteYield();
    }
    
    void ExplainQueryInfo::reviseN( long long n ) {
        verify( !_clauses.empty() );
        _clauses.back()->reviseN( n );
    }

    void ExplainQueryInfo::setAncillaryInfo( const AncillaryInfo &ancillaryInfo ) {
        _ancillaryInfo = ancillaryInfo;
    }
    
    BSONObj ExplainQueryInfo::bson() const {
        BSONObjBuilder bob;
        if ( _clauses.size() == 1 ) {
            bob.appendElements( _clauses.front()->bson() );
        }
        else {
            long long n = 0;
            long long nscannedObjects = 0;
            long long nscanned = 0;
            long long nscannedObjectsAllPlans = 0;
            long long nscannedAllPlans = 0;
            BSONArrayBuilder clauseArray( bob.subarrayStart( "clauses" ) );
            for( list<shared_ptr<ExplainClauseInfo> >::const_iterator i = _clauses.begin();
                i != _clauses.end(); ++i ) {
                clauseArray << (*i)->bson();
                n += (*i)->n();
                nscannedObjects += (*i)->nscannedObjects();
                nscanned += (*i)->nscanned();
                nscannedObjectsAllPlans += (*i)->nscannedObjectsAllPlans();
                nscannedAllPlans += (*i)->nscannedAllPlans();
            }
            clauseArray.done();
            bob.appendNumber( "n", n );
            bob.appendNumber( "nscannedObjects", nscannedObjects );
            bob.appendNumber( "nscanned", nscanned );
            bob.appendNumber( "nscannedObjectsAllPlans", nscannedObjectsAllPlans );
            bob.appendNumber( "nscannedAllPlans", nscannedAllPlans );
            bob.appendNumber( "millis", _timer.duration() );
        }
        
        if ( !_ancillaryInfo._oldPlan.isEmpty() ) {
            bob.append( "oldPlan", _ancillaryInfo._oldPlan );
        }
        bob.append( "server", server() );
        
        return bob.obj();
    }
    
    void ExplainQueryInfo::addClauseInfo( const shared_ptr<ExplainClauseInfo> &info ) {
        if ( !_clauses.empty() ) {
            _clauses.back()->stopTimer();
        }
        _clauses.push_back( info );
    }
    
    string ExplainQueryInfo::server() {
        return mongoutils::str::stream() << getHostNameCached() << ":" << cmdLine.port;
    }

    ExplainSinglePlanQueryInfo::ExplainSinglePlanQueryInfo() :
    _planInfo( new ExplainPlanInfo() ),
    _queryInfo( new ExplainQueryInfo() ) {
        shared_ptr<ExplainClauseInfo> clauseInfo( new ExplainClauseInfo() );
        clauseInfo->addPlanInfo( _planInfo );
        _queryInfo->addClauseInfo( clauseInfo );
    }
    
} // namespace mongo
