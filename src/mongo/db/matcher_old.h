// matcher_old.h

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

#include <pcrecpp.h>

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/matcher/match_details.h"

namespace mongo {

    class Cursor;
    class CoveredIndexMatcher;
    class FieldRangeVector;
    class MatcherOld;

    namespace old_matcher {

        class ElementMatcher;

        class RegexMatcher {
        public:
            /**
             * Maximum pattern size which pcre v8.3 can do matches correctly with
             * LINK_SIZE define macro set to 2 @ pcre's config.h (based on
             * experiments)
             */
            static const size_t MaxPatternSize = 32764;

            const char *_fieldName;
            const char *_regex;
            const char *_flags;
            string _prefix;
            shared_ptr< pcrecpp::RE > _re;
            bool _isNot;
            RegexMatcher() : _isNot() {}
        };

        struct GeoMatcher {
        public:
            GeoMatcher(GeoQuery query, bool negated) : geoQuery(query), isNot(negated) {}

            string getField() const { return geoQuery.getField(); }

            bool matches(const GeometryContainer &container) const {
                bool satisfied = geoQuery.satisfiesPredicate(container);
                if (isNot) { return !satisfied; }
                else { return satisfied; }
            }
            GeoQuery geoQuery;
            bool isNot;
        };

        /**
         * An interface for visiting a Matcher and all of its nested Matchers and ElementMatchers.
         * RegexMatchers are not visited.
         */
        class MatcherVisitor {
        public:
            virtual ~MatcherVisitor() {}
            virtual void visitMatcher( const MatcherOld& matcher ) {}
            virtual void visitElementMatcher( const ElementMatcher& elementMatcher ) {}
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

            void visit( MatcherVisitor& visitor ) const;

            BSONElement _toMatch;
            int _compareOp;
            bool _isNot;
            shared_ptr< set<BSONElement,element_lt> > _myset;
            shared_ptr< vector<RegexMatcher> > _myregex;

            // these are for specific operators
            int _mod;
            int _modm;
            BSONType _type;

            shared_ptr<MatcherOld> _subMatcher;
            bool _subMatcherOnPrimitives ;

            vector< shared_ptr<MatcherOld> > _allMatchers;
        };

        class Where; // used for $where javascript eval
        class DiskLoc;

    }

    /* Match BSON objects against a query pattern.

       e.g.
       db.foo.find( { a : 3 } );

       { a : 3 } is the pattern object.  See manual for full info.

       GT/LT:
       { a : { $gt : 3 } }
       Not equal:
       { a : { $ne : 3 } }

       TODO: we should rewrite the matcher to be more an AST style.
    */
    class MatcherOld : boost::noncopyable {
        int matchesDotted(
                          const char *fieldName,
                          const BSONElement& toMatch, const BSONObj& obj,
                          int compareOp, const old_matcher::ElementMatcher& bm, bool isArr , MatchDetails * details ) const;

        /**
         * Perform a NE or NIN match by returning the inverse of the opposite matching operation.
         * Missing values are considered matches unless the match must not equal null.
         */
        int inverseMatch(
                         const char *fieldName,
                         const BSONElement &toMatch, const BSONObj &obj,
                         const old_matcher::ElementMatcher&bm, MatchDetails * details ) const;

    public:
        static int opDirection(int op) {
            return op <= BSONObj::LTE ? -1 : 1;
        }

        MatcherOld(const BSONObj &pattern, bool nested=false);

        ~MatcherOld();

        bool matches(const BSONObj& j, MatchDetails * details = 0 ) const;

        /**
         * Visit this Matcher and all of its nested Matchers and ElementMatchers.  All top level
         * ElementMatchers of a Matcher are visited immediately after the Matcher itself (before any
         * other Matcher is visited).
         */
        void visit( old_matcher::MatcherVisitor& visitor ) const;

        bool atomic() const { return _atomic; }

        string toString() const {
            return _jsobj.toString();
        }

        /**
         * @return true if this key matcher will return the same true/false
         * value as the provided doc matcher.
         */
        bool keyMatch( const MatcherOld &docMatcher ) const;

        bool singleSimpleCriterion() const {
            if ( _where ||
                 _basics.size() > 1 ||
                 _haveNeg ||
                 _haveSize ||
                 _regexs.size() > 0 )
                return false;

            if ( _jsobj.nFields() > 1 )
                return false;

            if ( _basics.size() != 1 )
                return false;

            if ( strchr( _jsobj.firstElement().fieldName(), '.' ) )
                return false;

            return _basics[0]._compareOp == BSONObj::Equality;
        }

        const BSONObj *getQuery() const { return &_jsobj; };

        /**
         * Generate a matcher for the provided index key format using the
         * provided full doc matcher.
         */
        MatcherOld( const MatcherOld &docMatcher, const BSONObj &constrainIndexKey );

    private:

        void addBasic(const BSONElement &e, int c, bool isNot) {
            // TODO May want to selectively ignore these element types based on op type.
            if ( e.type() == MinKey || e.type() == MaxKey )
                return;
            _basics.push_back( old_matcher::ElementMatcher( e , c, isNot ) );
        }

        void addRegex(const char *fieldName, const char *regex, const char *flags, bool isNot = false);
        bool addOp( const BSONElement &e, const BSONElement &fe, bool isNot, const char *& regex, const char *&flags );

        int valuesMatch(const BSONElement& l, const BSONElement& r, int op, const old_matcher::ElementMatcher& bm) const;

        bool parseClause( const BSONElement &e );
        void parseExtractedClause( const BSONElement &e, list< shared_ptr< MatcherOld > > &matchers );

        void parseWhere( const BSONElement &e );
        void parseMatchExpressionElement( const BSONElement &e, bool nested );

        old_matcher::Where *_where;                    // set if query uses $where
        BSONObj _jsobj;                  // the query pattern.  e.g., { name: "joe" }
        BSONObj _constrainIndexKey;
        vector<old_matcher::ElementMatcher> _basics;
        bool _haveSize;
        bool _all;
        bool _hasArray;
        bool _haveNeg;

        /* $atomic - if true, a multi document operation (some removes, updates)
           should be done atomically.  in that case, we do not yield -
           i.e. we stay locked the whole time.
           http://dochub.mongodb.org/core/remove
        */
        bool _atomic;

        vector<old_matcher::RegexMatcher> _regexs;
        vector<old_matcher::GeoMatcher> _geo;

        // so we delete the mem when we're done:
        vector< shared_ptr< BSONObjBuilder > > _builders;
        list< shared_ptr< MatcherOld > > _andMatchers;
        list< shared_ptr< MatcherOld > > _orMatchers;
        list< shared_ptr< MatcherOld > > _norMatchers;

        friend class CoveredIndexMatcher;
    };

} // namespace mongo
