// matcher.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    class IndexKeyMatchableDocument : public MatchableDocument {
    public:
        IndexKeyMatchableDocument( const BSONObj& pattern,
                                   const BSONObj& doc )
            : _pattern( pattern ), _doc( doc ) {
        }

        BSONObj toBSON() const {
            // TODO: this isn't quite correct because of dots
            // don't think it'll ever be called though
            return _doc.replaceFieldNames( _pattern );
        }

        virtual BSONElement getFieldDottedOrArray( const FieldRef& path,
                                                   int32_t* idxPath,
                                                   bool* inArray ) const;

        virtual void getFieldsDotted( const StringData& name,
                                      BSONElementSet &ret,
                                      bool expandLastArray = true ) const;

    private:

        BSONElement _getElement( const FieldRef& path ) const;

        BSONObj _pattern;
        BSONObj _doc;

    };

    BSONElement IndexKeyMatchableDocument::_getElement( const FieldRef& path ) const {
        BSONObjIterator patternIterator( _pattern );
        BSONObjIterator docIterator( _pattern );

        while ( patternIterator.more() ) {
            BSONElement patternElement = patternIterator.next();
            verify( docIterator.more() );
            BSONElement docElement = docIterator.next();

            if ( path.equalsDottedField( patternElement.fieldName() ) ) {
                return docElement;
            }
        }

        return BSONElement();
    }


    BSONElement IndexKeyMatchableDocument::getFieldDottedOrArray( const FieldRef& path,
                                                                  int32_t* idxPath,
                                                                  bool* inArray ) const {
        BSONElement res = _getElement( path );
        if ( !res.eoo() ) {
            *idxPath = path.numParts() - 1;
            *inArray = false;
        }

        return res;
    }

    void IndexKeyMatchableDocument::getFieldsDotted( const StringData& name,
                                                     BSONElementSet &ret,
                                                     bool expandLastArray ) const {
        BSONObjIterator patternIterator( _pattern );
        BSONObjIterator docIterator( _pattern );

        while ( patternIterator.more() ) {
            BSONElement patternElement = patternIterator.next();
            verify( docIterator.more() );
            BSONElement docElement = docIterator.next();

            if ( name == patternElement.fieldName() ) {
                ret.insert( docElement );
            }
        }

    }


    // -----------------

    Matcher2::Matcher2( const BSONObj& pattern, bool nested )
        : _pattern( pattern ) {

        StatusWithMatchExpression result = MatchExpressionParser::parse( pattern );
        uassert( 16810,
                 mongoutils::str::stream() << "bad query: " << result.toString(),
                 result.isOK() );

        _expression.reset( result.getValue() );
    }

    Matcher2::Matcher2( const Matcher2 &docMatcher, const BSONObj &constrainIndexKey )
        : _indexKey( constrainIndexKey ) {

        MatchExpression* indexExpression = spliceForIndex( constrainIndexKey,
                                                          docMatcher._expression.get() );
        if ( indexExpression )
            _expression.reset( indexExpression );
    }

    bool Matcher2::matches(const BSONObj& doc, MatchDetails* details ) const {
        if ( !_expression )
            return true;

        if ( _indexKey.isEmpty() )
            return _expression->matchesBSON( doc, details );

        if ( !doc.isEmpty() && doc.firstElement().fieldName()[0] )
            return _expression->matchesBSON( doc, details );

        IndexKeyMatchableDocument mydoc( _indexKey, doc );
        return _expression->matches( &mydoc, details );
    }


    bool Matcher2::atomic() const {
        if ( !_expression )
            return false;

        if ( _expression->matchType() == MatchExpression::ATOMIC )
            return true;

        // we only go down one level
        for ( unsigned i = 0; i < _expression->numChildren(); i++ ) {
            if ( _expression->getChild( i )->matchType() == MatchExpression::ATOMIC )
                return true;
        }

        return false;
    }

    namespace {
        bool _isExistsFalse( const MatchExpression* e, bool negated ) {
            if ( e->matchType() == MatchExpression::EXISTS ){
                const ExistsMatchExpression* exists = static_cast<const ExistsMatchExpression*>(e);
                bool x = !exists->rightSideBool();
                if ( negated )
                    x = !x;
                return x;
            }

            if ( e->matchType() == MatchExpression::AND ||
                 e->matchType() == MatchExpression::OR ) {

                for ( unsigned i = 0; i < e->numChildren(); i++  ) {
                    if ( _isExistsFalse( e->getChild(i), negated ) )
                        return true;
                }
                return false;
            }

            if ( e->matchType() == MatchExpression::NOT )
                return _isExistsFalse( e->getChild(0), !negated );

            return false;
        }
    }

    bool Matcher2::hasExistsFalse() const {
        return _isExistsFalse( _expression.get(), false );
    }


    bool Matcher2::singleSimpleCriterion() const {
        if ( !_expression )
            return false;

        if ( _expression->matchType() == MatchExpression::EQ )
            return true;

        if ( _expression->matchType() == MatchExpression::AND &&
             _expression->numChildren() == 1 &&
             _expression->getChild(0)->matchType() == MatchExpression::EQ )
            return true;

        return false;
    }

    bool Matcher2::keyMatch( const Matcher2 &docMatcher ) const {
        if ( !_expression )
            return docMatcher._expression.get() == NULL;
        if ( !docMatcher._expression )
            return false;

        return _expression->equivalent( docMatcher._expression.get() );
    }

    MatchExpression* Matcher2::spliceForIndex( const BSONObj& key,
                                               const MatchExpression* full ) {
        set<string> keys;
        for ( BSONObjIterator i(key); i.more(); ) {
            BSONElement e = i.next();
            keys.insert( e.fieldName() );
        }
        return _spliceForIndex( keys, full );
    }

    MatchExpression* Matcher2::_spliceForIndex( const set<string>& keys,
                                                const MatchExpression* full ) {

        switch ( full->matchType() ) {
        case MatchExpression::NOT:
        case MatchExpression::NOR:
            // maybe?
            return NULL;

        case MatchExpression::AND:
        case MatchExpression::OR: {
            auto_ptr<ListOfMatchExpression> dup;
            for ( unsigned i = 0; i < full->numChildren(); i++ ) {
                MatchExpression* sub = _spliceForIndex( keys, full->getChild( i ) );
                if ( !sub )
                    continue;
                if ( !dup.get() ) {
                    if ( full->matchType() == MatchExpression::AND )
                        dup.reset( new AndMatchExpression() );
                    else
                        dup.reset( new OrMatchExpression() );
                }
                dup->add( sub );
            }
            if ( dup.get() )
                return dup.release();
            return NULL;
        }

        case MatchExpression::LTE:
        case MatchExpression::LT:
        case MatchExpression::EQ:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::NE:
        case MatchExpression::REGEX:
        case MatchExpression::MOD:
        case MatchExpression::MATCH_IN: {
            const LeafMatchExpression* lme = static_cast<const LeafMatchExpression*>( full );
            if ( !keys.count( lme->getPath().toString() ) )
                return NULL;
            return lme->shallowClone();
        }

        case MatchExpression::ALL:
            // TODO: conver to $in
            return NULL;

        case MatchExpression::ELEM_MATCH_OBJECT:
        case MatchExpression::ELEM_MATCH_VALUE:
            // future
            return NULL;

        case MatchExpression::GEO:
        case MatchExpression::SIZE:
        case MatchExpression::EXISTS:
        case MatchExpression::NIN:
        case MatchExpression::TYPE_OPERATOR:
        case MatchExpression::ATOMIC:
        case MatchExpression::WHERE:
            // no go
            return NULL;


        }

        return NULL;
    }

}
