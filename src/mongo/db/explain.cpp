// @file explain.h - Helper classes for generating query explain output.

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

#include "explain.h"

#include "cmdline.h"
#include "../util/net/sock.h"
#include "../util/mongoutils/str.h"

namespace mongo {
    
    // TODO get rid of const casts

    ExplainPlanInfo::ExplainPlanInfo() :
    _isMultiKey(),
    _n(),
    _nscannedObjects(),
    _nscanned(),
    _scanAndOrder(),
    _nYields(),
    _picked(),
    _done() {
    }

    void ExplainPlanInfo::notePlan( const Cursor &cursor, bool scanAndOrder ) {
        _cursorName = const_cast<Cursor&>(cursor).toString();
        _indexBounds = cursor.prettyIndexBounds();
        _scanAndOrder = scanAndOrder;
        noteCursorUpdate( cursor );
    }
    
    void ExplainPlanInfo::noteIterate( bool match, bool loadedObject, const Cursor &cursor ) {
        if ( match ) {
            ++_n;
        }
        if ( loadedObject ) {
            ++_nscannedObjects;
        }
        noteCursorUpdate( cursor );
    }
    
    void ExplainPlanInfo::noteYield() { ++_nYields; }
    
    void ExplainPlanInfo::noteDone( const Cursor &cursor ) {
        _done = true;
        noteCursorUpdate( cursor );
    }
    
    void ExplainPlanInfo::notePicked() {
        _picked = true;
    }

    BSONObj ExplainPlanInfo::bson() const {
        return BSON(
                    "cursor" << _cursorName <<
                    "n" << _n <<
                    "nscannedObjects" << _nscannedObjects <<
                    "nscanned" << _nscanned <<
                    "indexBounds" << _indexBounds
                    );
    }
    
    BSONObj ExplainPlanInfo::pickedPlanBson( const ExplainClauseInfo &clauseInfo ) const {
        return BSON(
                    "cursor" << _cursorName <<
                    "isMultiKey" << _isMultiKey <<
                    "n" << clauseInfo.n() <<
                    "nscannedObjects" << clauseInfo.nscannedObjects() <<
                    "nscanned" << clauseInfo.nscanned() <<
                    "scanAndOrder" << _scanAndOrder <<
                    "indexOnly" << false << // TODO
                    "nYields" << _nYields <<
                    "nChunkSkips" << clauseInfo.nChunkSkips() <<
                    "millis" << clauseInfo.millis() <<
                    "indexBounds" << _indexBounds
                    );
    }

    void ExplainPlanInfo::noteCursorUpdate( const Cursor &cursor ) {
        _isMultiKey = cursor.isMultiKey();
        _nscanned = const_cast<Cursor&>(cursor).nscanned();
    }

    ExplainClauseInfo::ExplainClauseInfo() :
    _n(),
    _nscannedObjects(),
    _nChunkSkips() {
    }
    
    BSONObj ExplainClauseInfo::bson() const {
        BSONObjBuilder bb;
        bb.appendElements( virtualPickedPlan().pickedPlanBson( *this ) );
        // TODO won't include plans w/ no cursor iterates.
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
    
    void ExplainClauseInfo::noteIterate( bool match, bool loadedObject, bool chunkSkip ) {
        if ( match ) {
            ++_n;
        }
        if ( loadedObject ) {
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

    long long ExplainClauseInfo::nscanned() const {
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
        int maxN = 0;
        for( list<shared_ptr<const ExplainPlanInfo> >::const_iterator i = _plans.begin();
            i != _plans.end(); ++i ) {
            if ( (*i)->n() > maxN ) {
                maxN = (*i)->n();
            }
        }
        for( list<shared_ptr<const ExplainPlanInfo> >::const_iterator i = _plans.begin();
            i != _plans.end(); ++i ) {
            if ( (*i)->n() == maxN ) {
                return **i;
            }
        }
        verify( 16076, false );
        return *(new ExplainPlanInfo());  // TODO better
    }
    
    void ExplainQueryInfo::noteIterate( bool match, bool loadedObject, bool chunkSkip ) {
        verify( 16077, !_clauses.empty() );
        _clauses.back()->noteIterate( match, loadedObject, chunkSkip );
    }

    void ExplainQueryInfo::reviseN( long long n ) {
        verify( 16073, !_clauses.empty() );
        _clauses.back()->reviseN( n );
    }

    void ExplainQueryInfo::setAncillaryInfo( const AncillaryInfo &ancillaryInfo ) {
        _ancillaryInfo = ancillaryInfo;
    }
    
    BSONObj ExplainQueryInfo::bson() const {
        BSONObjBuilder bb;
        if ( _clauses.size() == 1 ) {
            bb.appendElements( _clauses.front()->bson() );
        }
        else {
            long long n = 0;
            long long nscannedObjects = 0;
            long long nscanned = 0;
            BSONArrayBuilder clauseArray( bb.subarrayStart( "clauses" ) );
            for( list<shared_ptr<ExplainClauseInfo> >::const_iterator i = _clauses.begin();
                i != _clauses.end(); ++i ) {
                clauseArray << (*i)->bson();
                n += (*i)->n();
                nscannedObjects += (*i)->nscannedObjects();
                nscanned += (*i)->nscanned();
            }
            clauseArray.done();
            bb
            << "n" << n
            << "nscannedObjects" << nscannedObjects
            << "nscanned" << nscanned
            << "millis" << _timer.duration();
        }
        
        if ( !_ancillaryInfo._oldPlan.isEmpty() ) {
            bb << "oldPlan" << _ancillaryInfo._oldPlan;
        }
        bb
        << "server"
        << (string)( mongoutils::str::stream() << getHostNameCached() << ":" << cmdLine.port );
        
        return bb.obj();
    }
    
    void ExplainQueryInfo::addClauseInfo( const shared_ptr<ExplainClauseInfo> &info ) {
        if ( !_clauses.empty() ) {
            _clauses.back()->stopTimer();
        }
        _clauses.push_back( info );
    }

    ExplainSinglePlanQueryInfo::ExplainSinglePlanQueryInfo() :
    _planInfo( new ExplainPlanInfo() ),
    _queryInfo( new ExplainQueryInfo() ) {
        shared_ptr<ExplainClauseInfo> clauseInfo( new ExplainClauseInfo() );
        clauseInfo->addPlanInfo( _planInfo );
        _queryInfo->addClauseInfo( clauseInfo );
    }
    
} // namespace mongo
