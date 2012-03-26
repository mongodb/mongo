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
#include "pcrecpp.h"

namespace mongo {

    class Cursor;
    class CoveredIndexMatcher;
    class Matcher;
    class FieldRangeVector;

    class RegexMatcher {
    public:
        const char *_fieldName;
        const char *_regex;
        const char *_flags;
        string _prefix;
        shared_ptr< pcrecpp::RE > _re;
        bool _isNot;
        RegexMatcher() : _isNot() {}
    };

    struct element_lt {
        bool operator()(const BSONElement& l, const BSONElement& r) const {
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

        ElementMatcher( BSONElement e , int op, bool isNot );

        ElementMatcher( BSONElement e , int op , const BSONObj& array, bool isNot );

        ~ElementMatcher() { }
        
        bool negativeCompareOp() const { return _compareOp == BSONObj::NE || _compareOp == BSONObj::NIN; }
        int inverseOfNegativeCompareOp() const;
        bool negativeCompareOpContainsNull() const;
        
        BSONElement _toMatch;
        int _compareOp;
        bool _isNot;
        shared_ptr< set<BSONElement,element_lt> > _myset;
        shared_ptr< vector<RegexMatcher> > _myregex;

        // these are for specific operators
        int _mod;
        int _modm;
        BSONType _type;

        shared_ptr<Matcher> _subMatcher;
        bool _subMatcherOnPrimitives ;

        vector< shared_ptr<Matcher> > _allMatchers;
    };

    class Where; // used for $where javascript eval
    class DiskLoc;

    /** Reports information about a match request. */
    class MatchDetails {
    public:
        MatchDetails();
        void resetOutput();
        string toString() const;

        /** Request that an elemMatchKey be recorded. */
        void requestElemMatchKey() { _elemMatchKeyRequested = true; }
        
        bool needRecord() const { return _elemMatchKeyRequested; }
        
        bool hasLoadedRecord() const { return _loadedRecord; }
        bool hasElemMatchKey() const { return _elemMatchKeyFound; }
        string elemMatchKey() const {
            verify( hasElemMatchKey() );
            return _elemMatchKey;
        }

        void setLoadedRecord( bool loadedRecord ) { _loadedRecord = loadedRecord; }
        void setElemMatchKey( const string &elemMatchKey ) {
            if ( _elemMatchKeyRequested ) {
                _elemMatchKeyFound = true;
                _elemMatchKey = elemMatchKey;
            }
        }
        
    private:
        bool _loadedRecord;
        bool _elemMatchKeyRequested;
        bool _elemMatchKeyFound;
        string _elemMatchKey;
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
            int compareOp, const ElementMatcher& bm, bool isArr , MatchDetails * details ) const;

        /**
         * Perform a NE or NIN match by returning the inverse of the opposite matching operation.
         * Missing values are considered matches unless the match must not equal null.
         */
        int inverseMatch(
            const char *fieldName,
            const BSONElement &toMatch, const BSONObj &obj,
            const ElementMatcher&bm, MatchDetails * details ) const;

    public:
        static int opDirection(int op) {
            return op <= BSONObj::LTE ? -1 : 1;
        }

        Matcher(const BSONObj &pattern, bool nested=false);

        ~Matcher();

        bool matches(const BSONObj& j, MatchDetails * details = 0 ) const;

#ifdef MONGO_LATER_SERVER_4644
        class FieldSink {
        public:
            virtual ~FieldSink() {};
            virtual void referenceField(const string &fieldPath) = 0;
        };

        /**
           Visit all of the fields that are referenced by this Matcher
           (and any descendants).

           This can be used to gather a list of all the references made by
           this matcher.  The implementation of this parallels that of
           matches() above.

           @param pSink a FieldSink that the caller will use to gather or
             process the references
         */
        void visitReferences(FieldSink *pSink) const;
#endif /* MONGO_LATER_SERVER_4644 */

        bool atomic() const { return _atomic; }

        string toString() const {
            return _jsobj.toString();
        }

        void addOrDedupConstraint( const shared_ptr< FieldRangeVector > &frv ) {
            _orDedupConstraints.push_back( frv );
        }

        void popOrClause() {
            _orMatchers.pop_front();
        }

        /**
         * @return true if this key matcher will return the same true/false
         * value as the provided doc matcher.
         */
        bool keyMatch( const Matcher &docMatcher ) const;
        
        bool singleSimpleCriterion() const {
            return false; // TODO SERVER-958
//            // TODO Really check, especially if all basics are ok.
//            // $all, etc
//            // _orConstraints?
//            return ( ( basics.size() + nRegex ) < 2 ) && !where && !_orMatchers.size() && !_norMatchers.size();
        }

        const BSONObj *getQuery() const { return &_jsobj; };

    private:
        /**
         * Generate a matcher for the provided index key format using the
         * provided full doc matcher.
         */
        Matcher( const Matcher &docMatcher, const BSONObj &constrainIndexKey );

        void addBasic(const BSONElement &e, int c, bool isNot) {
            // TODO May want to selectively ignore these element types based on op type.
            if ( e.type() == MinKey || e.type() == MaxKey )
                return;
            _basics.push_back( ElementMatcher( e , c, isNot ) );
        }

        void addRegex(const char *fieldName, const char *regex, const char *flags, bool isNot = false);
        bool addOp( const BSONElement &e, const BSONElement &fe, bool isNot, const char *& regex, const char *&flags );

        int valuesMatch(const BSONElement& l, const BSONElement& r, int op, const ElementMatcher& bm) const;

        bool parseClause( const BSONElement &e );
        void parseExtractedClause( const BSONElement &e, list< shared_ptr< Matcher > > &matchers );

        void parseWhere( const BSONElement &e );
        void parseMatchExpressionElement( const BSONElement &e, bool nested );
        
        Where *_where;                    // set if query uses $where
        BSONObj _jsobj;                  // the query pattern.  e.g., { name: "joe" }
        BSONObj _constrainIndexKey;
        vector<ElementMatcher> _basics;
        bool _haveSize;
        bool _all;
        bool _hasArray;
        bool _haveNeg;

        /* $atomic - if true, a multi document operation (some removes, updates)
                     should be done atomically.  in that case, we do not yield -
                     i.e. we stay locked the whole time.
                     http://www.mongodb.org/display/DOCS/Removing[
        */
        bool _atomic;

        vector<RegexMatcher> _regexs;

        // so we delete the mem when we're done:
        vector< shared_ptr< BSONObjBuilder > > _builders;
        list< shared_ptr< Matcher > > _andMatchers;
        list< shared_ptr< Matcher > > _orMatchers;
        list< shared_ptr< Matcher > > _norMatchers;
        vector< shared_ptr< FieldRangeVector > > _orDedupConstraints;

        friend class CoveredIndexMatcher;
    };

    // If match succeeds on index key, then attempt to match full document.
    class CoveredIndexMatcher : boost::noncopyable {
    public:
        CoveredIndexMatcher(const BSONObj &pattern, const BSONObj &indexKeyPattern);
        bool matchesWithSingleKeyIndex(const BSONObj &key, const DiskLoc &recLoc , MatchDetails * details = 0 ) {
            return matches( key, recLoc, details, true );   
        }
        /**
         * This is the preferred method for matching against a cursor, as it
         * can handle both multi and single key cursors.
         */
        bool matchesCurrent( Cursor * cursor , MatchDetails * details = 0 );
        bool needRecord() { return _needRecord; }

        Matcher& docMatcher() { return *_docMatcher; }

        // once this is called, shouldn't use this matcher for matching any more
        void advanceOrClause( const shared_ptr< FieldRangeVector > &frv ) {
            _docMatcher->addOrDedupConstraint( frv );
            // TODO this is not yet optimal.  Since we could skip an entire
            // or clause (if a match is impossible) between calls to advanceOrClause()
            // we may not pop all the clauses we can.
            _docMatcher->popOrClause();
        }

        CoveredIndexMatcher *nextClauseMatcher( const BSONObj &indexKeyPattern ) {
            return new CoveredIndexMatcher( _docMatcher, indexKeyPattern );
        }

        string toString() const;

    private:
        bool matches(const BSONObj &key, const DiskLoc &recLoc , MatchDetails * details = 0 , bool keyUsable = true );
        CoveredIndexMatcher(const shared_ptr< Matcher > &docMatcher, const BSONObj &indexKeyPattern);
        void init();
        shared_ptr< Matcher > _docMatcher;
        Matcher _keyMatcher;

        bool _needRecord; // if the key itself isn't good enough to determine a positive match
    };

} // namespace mongo
