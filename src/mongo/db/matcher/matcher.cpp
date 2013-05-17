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

#include "mongo/base/init.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

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
                                                   size_t* idxPath,
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
        BSONObjIterator docIterator( _doc );

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
                                                                  size_t* idxPath,
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
                                                           docMatcher._expression.get(),
                                                           &_spliceInfo );
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
        bool _isExistsFalse( const MatchExpression* e, bool negated, int depth ) {
            if ( e->matchType() == MatchExpression::EXISTS ){
                if ( depth > 0 )
                    return true;
                return negated;
            }

            if ( e->matchType() == MatchExpression::AND ||
                 e->matchType() == MatchExpression::NOR ||
                 e->matchType() == MatchExpression::OR ) {

                for ( unsigned i = 0; i < e->numChildren(); i++  ) {
                    if ( _isExistsFalse( e->getChild(i), negated, depth + 1 ) )
                        return true;
                }
                return false;
            }

            if ( e->matchType() == MatchExpression::NOT ) {
                if ( e->getChild(0)->matchType() == MatchExpression::AND )
                    depth--;
                return _isExistsFalse( e->getChild(0), !negated, depth );
            }

            return false;
        }
    }

    bool Matcher2::hasExistsFalse() const {
        return _isExistsFalse( _expression.get(), false,
                               _expression->matchType() == MatchExpression::AND ? -1 : 0 );
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
        if ( _spliceInfo.hasNullEquality )
            return false;
        return _expression->equivalent( docMatcher._expression.get() );
    }

    MatchExpression* Matcher2::spliceForIndex( const BSONObj& key,
                                               const MatchExpression* full,
                                               Matcher2::IndexSpliceInfo* spliceInfo ) {
        set<string> keys;
        for ( BSONObjIterator i(key); i.more(); ) {
            BSONElement e = i.next();
            keys.insert( e.fieldName() );
        }
        return _spliceForIndex( keys, full, spliceInfo );
    }

    namespace {
        BSONObj myUndefinedObj;
        BSONElement myUndefinedElement;

        MONGO_INITIALIZER( MatcherUndefined )( ::mongo::InitializerContext* context ) {
            BSONObjBuilder b;
            b.appendUndefined( "a" );
            myUndefinedObj = b.obj();
            myUndefinedElement = myUndefinedObj["a"];
            return Status::OK();
        }

    }



    MatchExpression* Matcher2::_spliceForIndex( const set<string>& keys,
                                                const MatchExpression* full,
                                                Matcher2::IndexSpliceInfo* spliceInfo  ) {

        switch ( full->matchType() ) {
        case MatchExpression::NOT:
        case MatchExpression::NOR:
            // maybe?
            return NULL;

        case MatchExpression::OR:
        case MatchExpression::AND: {
            auto_ptr<ListOfMatchExpression> dup;
            for ( unsigned i = 0; i < full->numChildren(); i++ ) {
                MatchExpression* sub = _spliceForIndex( keys, full->getChild( i ), spliceInfo );
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

        case MatchExpression::EQ: {
            const ComparisonMatchExpression* cmp =
                static_cast<const ComparisonMatchExpression*>( full );

            if ( cmp->getRHS().type() == Array ) {
                // need to convert array to an $in

                if ( !keys.count( cmp->path().toString() ) )
                    return NULL;

                auto_ptr<InMatchExpression> newIn( new InMatchExpression() );
                newIn->init( cmp->path() );

                if ( newIn->getArrayFilterEntries()->addEquality( cmp->getRHS() ).isOK() )
                    return NULL;

                if ( cmp->getRHS().Obj().isEmpty() )
                    newIn->getArrayFilterEntries()->addEquality( myUndefinedElement );

                BSONObjIterator i( cmp->getRHS().Obj() );
                while ( i.more() ) {
                    Status s = newIn->getArrayFilterEntries()->addEquality( i.next() );
                    if ( !s.isOK() )
                        return NULL;
                }

                return newIn.release();
            }
            else if ( cmp->getRHS().type() == jstNULL ) {
                spliceInfo->hasNullEquality = true;
            }
        }

        case MatchExpression::LTE:
        case MatchExpression::LT:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::NE:
        case MatchExpression::REGEX:
        case MatchExpression::MOD: {
            const LeafMatchExpression* lme = static_cast<const LeafMatchExpression*>( full );
            if ( !keys.count( lme->path().toString() ) )
                return NULL;
            return lme->shallowClone();
        }

        case MatchExpression::MATCH_IN: {
            const LeafMatchExpression* lme = static_cast<const LeafMatchExpression*>( full );
            if ( !keys.count( lme->path().toString() ) )
                return NULL;
            InMatchExpression* cloned = static_cast<InMatchExpression*>(lme->shallowClone());
            if ( cloned->getArrayFilterEntries()->hasEmptyArray() )
                cloned->getArrayFilterEntries()->addEquality( myUndefinedElement );
            return cloned;
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
