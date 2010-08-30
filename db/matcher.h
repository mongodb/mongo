// matcher.h

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

#pragma once

#include "jsobj.h"
#include <pcrecpp.h>

namespace mongo {
    
    class Cursor;
    class CoveredIndexMatcher;
    class Matcher;
    class FieldRangeVector;

    class RegexMatcher {
    public:
        const char *fieldName;
        const char *regex;
        const char *flags;
        string prefix;
        shared_ptr< pcrecpp::RE > re;
        bool isNot;
        RegexMatcher() : isNot() {}
    };
    
    struct element_lt
    {
        bool operator()(const BSONElement& l, const BSONElement& r) const
        {
            int x = (int) l.canonicalType() - (int) r.canonicalType();
            if ( x < 0 ) return true;
            else if ( x > 0 ) return false;
            return compareElementValues(l,r) < 0;
        }
    };

    
    class ElementMatcher {
    public:
    
        ElementMatcher() {
        }
        
        ElementMatcher( BSONElement _e , int _op, bool _isNot );
        
        ElementMatcher( BSONElement _e , int _op , const BSONObj& array, bool _isNot );
        
        ~ElementMatcher() { }

        BSONElement toMatch;
        int compareOp;
        bool isNot;
        shared_ptr< set<BSONElement,element_lt> > myset;
        shared_ptr< vector<RegexMatcher> > myregex;
        
        // these are for specific operators
        int mod;
        int modm;
        BSONType type;

        shared_ptr<Matcher> subMatcher;

        vector< shared_ptr<Matcher> > allMatchers;
    };

    class Where; // used for $where javascript eval
    class DiskLoc;

    struct MatchDetails {
        MatchDetails(){
            reset();
        }
        
        void reset(){
            loadedObject = false;
            elemMatchKey = 0;
        }
        
        string toString() const {
            stringstream ss;
            ss << "loadedObject: " << loadedObject << " ";
            ss << "elemMatchKey: " << ( elemMatchKey ? elemMatchKey : "NULL" ) << " ";
            return ss.str();
        }

        bool loadedObject;
        const char * elemMatchKey; // warning, this may go out of scope if matched object does
    };

    /* Match BSON objects against a query pattern.

       e.g.
           db.foo.find( { a : 3 } );

       { a : 3 } is the pattern object.  See wiki documentation for full info.

       GT/LT:
         { a : { $gt : 3 } }
       Not equal:
         { a : { $ne : 3 } }

       TODO: we should rewrite the matcher to be more an AST style.
    */
    class Matcher : boost::noncopyable {
        int matchesDotted(
            const char *fieldName,
            const BSONElement& toMatch, const BSONObj& obj,
            int compareOp, const ElementMatcher& bm, bool isArr , MatchDetails * details );

        int matchesNe(
            const char *fieldName,
            const BSONElement &toMatch, const BSONObj &obj,
            const ElementMatcher&bm, MatchDetails * details );
        
    public:
        static int opDirection(int op) {
            return op <= BSONObj::LTE ? -1 : 1;
        }

        Matcher(const BSONObj &pattern, bool subMatcher = false);

        ~Matcher();

        bool matches(const BSONObj& j, MatchDetails * details = 0 );
        
        // fast rough check to see if we must load the real doc - we also
        // compare field counts against covereed index matcher; for $or clauses
        // we just compare field counts
        bool keyMatch() const { return !all && !haveSize && !hasArray && !haveNeg; }

        bool atomic() const { return _atomic; }
        
        bool hasType( BSONObj::MatchType type ) const;

        string toString() const {
            return jsobj.toString();
        }

        void addOrConstraint( const shared_ptr< FieldRangeVector > &frv ) {
            _orConstraints.push_back( frv );
        }
        
        void popOrClause() {
            _orMatchers.pop_front();
        }
        
        bool sameCriteriaCount( const Matcher &other ) const;
        
    private:
        // Only specify constrainIndexKey if matches() will be called with
        // index keys having empty string field names.
        Matcher( const Matcher &other, const BSONObj &constrainIndexKey );
        
        void addBasic(const BSONElement &e, int c, bool isNot) {
            // TODO May want to selectively ignore these element types based on op type.
            if ( e.type() == MinKey || e.type() == MaxKey )
                return;
            basics.push_back( ElementMatcher( e , c, isNot ) );
        }

        void addRegex(const char *fieldName, const char *regex, const char *flags, bool isNot = false);
        bool addOp( const BSONElement &e, const BSONElement &fe, bool isNot, const char *& regex, const char *&flags );
        
        int valuesMatch(const BSONElement& l, const BSONElement& r, int op, const ElementMatcher& bm);

        bool parseOrNor( const BSONElement &e, bool subMatcher );
        void parseOr( const BSONElement &e, bool subMatcher, list< shared_ptr< Matcher > > &matchers );

        Where *where;                    // set if query uses $where
        BSONObj jsobj;                  // the query pattern.  e.g., { name: "joe" }
        BSONObj constrainIndexKey_;
        vector<ElementMatcher> basics;
        bool haveSize;
        bool all;
        bool hasArray;
        bool haveNeg;

        /* $atomic - if true, a multi document operation (some removes, updates)
                     should be done atomically.  in that case, we do not yield - 
                     i.e. we stay locked the whole time.
                     http://www.mongodb.org/display/DOCS/Removing[
        */
        bool _atomic;

        RegexMatcher regexs[4];
        int nRegex;

        // so we delete the mem when we're done:
        vector< shared_ptr< BSONObjBuilder > > _builders;
        list< shared_ptr< Matcher > > _orMatchers;
        list< shared_ptr< Matcher > > _norMatchers;
        vector< shared_ptr< FieldRangeVector > > _orConstraints;

        friend class CoveredIndexMatcher;
    };
    
    // If match succeeds on index key, then attempt to match full document.
    class CoveredIndexMatcher : boost::noncopyable {
    public:
        CoveredIndexMatcher(const BSONObj &pattern, const BSONObj &indexKeyPattern , bool alwaysUseRecord=false );
        bool matches(const BSONObj &o){ return _docMatcher->matches( o ); }
        bool matches(const BSONObj &key, const DiskLoc &recLoc , MatchDetails * details = 0 );
        bool matchesCurrent( Cursor * cursor , MatchDetails * details = 0 );
        bool needRecord(){ return _needRecord; }
        
        Matcher& docMatcher() { return *_docMatcher; }

        // once this is called, shouldn't use this matcher for matching any more
        void advanceOrClause( const shared_ptr< FieldRangeVector > &frv ) {
            _docMatcher->addOrConstraint( frv );
            // TODO this is not an optimal optimization, since we could skip an entire
            // or clause (if a match is impossible) between calls to advanceOrClause()
            _docMatcher->popOrClause();
        }
        
        CoveredIndexMatcher *nextClauseMatcher( const BSONObj &indexKeyPattern, bool alwaysUseRecord=false ) {
            return new CoveredIndexMatcher( _docMatcher, indexKeyPattern, alwaysUseRecord );
        }
    private:
        CoveredIndexMatcher(const shared_ptr< Matcher > &docMatcher, const BSONObj &indexKeyPattern , bool alwaysUseRecord=false );
        void init( bool alwaysUseRecord );
        shared_ptr< Matcher > _docMatcher;
        Matcher _keyMatcher;
        bool _needRecord;
        bool _useRecordOnly;
    };
    
} // namespace mongo
