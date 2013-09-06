// matcher_covered.cpp

/* Matcher is our boolean expression evaluator for "where" clauses */

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

#include "mongo/db/cursor.h"
#include "mongo/db/matcher.h"
#include "mongo/db/matcher_covered.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/queryutil.h"

namespace mongo {

    CoveredIndexMatcher::CoveredIndexMatcher( const BSONObj &jsobj,
                                             const BSONObj &indexKeyPattern ) :
        _docMatcher( new Matcher( jsobj ) ),
        _keyMatcher( *_docMatcher, indexKeyPattern ) {
        init();
    }

    CoveredIndexMatcher::CoveredIndexMatcher( const CoveredIndexMatcher &prevClauseMatcher,
                                             const shared_ptr<FieldRangeVector> &prevClauseFrv,
                                             const BSONObj &nextClauseIndexKeyPattern ) :
        _docMatcher( prevClauseMatcher._docMatcher ),
        _keyMatcher( *_docMatcher, nextClauseIndexKeyPattern ),
        _orDedupConstraints( prevClauseMatcher._orDedupConstraints ) {
        if ( prevClauseFrv ) {
            _orDedupConstraints.push_back( prevClauseFrv );
        }
        init();
    }

    void CoveredIndexMatcher::init() {
        _needRecord =
            !_keyMatcher.keyMatch( *_docMatcher ) ||
            !_orDedupConstraints.empty();
    }

    bool CoveredIndexMatcher::matchesCurrent( Cursor * cursor , MatchDetails * details ) const {
        // bool keyUsable = ! cursor->isMultiKey() && check for $orish like conditions in matcher SERVER-1264

        bool keyUsable = true;
        if ( cursor->indexKeyPattern().isEmpty() ) { // unindexed cursor
            keyUsable = false;
        }
        else if ( cursor->isMultiKey() ) {
            keyUsable =
                _keyMatcher.singleSimpleCriterion() &&
                ( ! _docMatcher || _docMatcher->singleSimpleCriterion() );
        }
        return matches( cursor->currKey(),
                        cursor->currLoc(),
                        details,
                        keyUsable );
    }

    bool CoveredIndexMatcher::matches( const BSONObj& key, const DiskLoc& recLoc,
                                       MatchDetails* details, bool keyUsable ) const {

        LOG(5) << "CoveredIndexMatcher::matches() " << key.toString() << ' ' << recLoc.toString() << ' ' << keyUsable << endl;

        dassert( key.isValid() );

        if ( details )
            details->resetOutput();

        if ( keyUsable ) {
            if ( !_keyMatcher.matches(key, details ) ) {
                return false;
            }
            bool needRecordForDetails = details && details->needRecord();
            if ( !_needRecord && !needRecordForDetails ) {
                return true;
            }
        }

        BSONObj obj = recLoc.obj();
        bool res =
            _docMatcher->matches( obj, details ) &&
            !isOrClauseDup( obj );

        if ( details )
            details->setLoadedRecord( true );

        LOG(5) << "CoveredIndexMatcher _docMatcher->matches() returns " << res << endl;
        return res;
    }
    
    bool CoveredIndexMatcher::isOrClauseDup( const BSONObj &obj ) const {
        for( vector<shared_ptr<FieldRangeVector> >::const_iterator i = _orDedupConstraints.begin();
            i != _orDedupConstraints.end(); ++i ) {
            if ( (*i)->matches( obj ) ) {
                // If a document matches a prior $or clause index range, generally it would have
                // been returned while scanning that range and so is reported as a dup.
                return true;
            }
        }
        return false;
    }

    string CoveredIndexMatcher::toString() const {
        StringBuilder buf;
        buf << "(CoveredIndexMatcher ";
        
        if ( _needRecord )
            buf << "needRecord ";
        
        buf << "keyMatcher: " << _keyMatcher.toString() << " ";
        
        if ( _docMatcher )
            buf << "docMatcher: " << _docMatcher->toString() << " ";
        
        buf << ")";
        return buf.str();
    }
}
