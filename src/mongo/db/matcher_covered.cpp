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
*/

#include "pch.h"
#include "matcher.h"
#include "../util/goodies.h"
#include "diskloc.h"
#include "../scripting/engine.h"
#include "db.h"
#include "client.h"

#include "pdfile.h"

namespace mongo {

    CoveredIndexMatcher::CoveredIndexMatcher( const BSONObj &jsobj, const BSONObj &indexKeyPattern ) :
        _docMatcher( new Matcher( jsobj ) ),
        _keyMatcher( *_docMatcher, indexKeyPattern ) {
        init();
    }

    CoveredIndexMatcher::CoveredIndexMatcher( const shared_ptr< Matcher > &docMatcher, const BSONObj &indexKeyPattern ) :
        _docMatcher( docMatcher ),
        _keyMatcher( *_docMatcher, indexKeyPattern ) {
        init();
    }

    void CoveredIndexMatcher::init() {
        _needRecord = !_keyMatcher.keyMatch( *_docMatcher );
    }

    bool CoveredIndexMatcher::matchesCurrent( Cursor * cursor , MatchDetails * details ) {
        // bool keyUsable = ! cursor->isMultiKey() && check for $orish like conditions in matcher SERVER-1264
        return matches( cursor->currKey() , cursor->currLoc() , details ,
                       !cursor->indexKeyPattern().isEmpty() // unindexed cursor
                       && !cursor->isMultiKey() // multikey cursor
                       );
    }

    bool CoveredIndexMatcher::matches(const BSONObj &key, const DiskLoc &recLoc , MatchDetails * details , bool keyUsable ) {

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

        if ( details )
            details->setLoadedRecord( true );

        bool res = _docMatcher->matches(recLoc.obj() , details );
        LOG(5) << "CoveredIndexMatcher _docMatcher->matches() returns " << res << endl;
        return res;
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
