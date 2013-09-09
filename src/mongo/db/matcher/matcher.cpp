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

#include "mongo/base/init.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/matcher/path.h"
#include "mongo/db/exec/working_set.h"
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

        virtual ElementIterator* allocateIterator( const ElementPath* path ) const;

        virtual void releaseIterator( ElementIterator* iterator ) const {
            delete iterator;
        }

    private:

        BSONElement _getElement( const FieldRef& path ) const;

        BSONObj _pattern;
        BSONObj _doc;
    };

    ElementIterator* IndexKeyMatchableDocument::allocateIterator( const ElementPath* path ) const {
        BSONElement e = _getElement( path->fieldRef() );
        if ( e.type() == Array )
            return new SimpleArrayElementIterator( e, true );
        return new SingleElementElementIterator( e );
    }


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
        if ( indexExpression ) {
            _expression.reset( indexExpression );
        }
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
            switch( e->matchType() ) {
            case MatchExpression::EXISTS: {
                if ( depth > 0 )
                    return true;
                // i'm "good" unless i'm negated
                return negated;
            }

            case MatchExpression::NOT: {
                if ( e->getChild(0)->matchType() == MatchExpression::AND )
                    depth--;
                return _isExistsFalse( e->getChild(0), !negated, depth );
            }

            case MatchExpression::EQ: {
                const ComparisonMatchExpression* cmp =
                    static_cast<const ComparisonMatchExpression*>( e );
                if ( cmp->getRHS().type() == jstNULL ) {
                    // i'm "bad" unless i'm negated
                    return !negated;
                }
            }

            default:
                for ( unsigned i = 0; i < e->numChildren(); i++  ) {
                    if ( _isExistsFalse( e->getChild(i), negated, depth + 1 ) )
                        return true;
                }
                return false;
            }
            return false;
        }
    }

    bool Matcher2::hasExistsFalse() const {
        if ( _spliceInfo.hasNullEquality ) {
            // { a : NULL } is very dangerous as it may not got indexed in some cases
            // so we just totally ignore
            return true;
        }

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
        case MatchExpression::ALWAYS_FALSE:
            return new FalseMatchExpression();

        case MatchExpression::GEO_NEAR:
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
            if ( dup.get() ) {
                if ( full->matchType() == MatchExpression::OR &&
                     dup->numChildren() != full->numChildren() ) {
                    // TODO: I think this should actuall get a list of all the fields
                    // and make sure that's the same
                    // with an $or, have to make sure its all or nothing
                    return NULL;
                }
                return dup.release();
            }
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
                //spliceInfo->hasNullEquality = true;
                return NULL;
            }
        }

        case MatchExpression::LTE:
        case MatchExpression::LT:
        case MatchExpression::GT:
        case MatchExpression::GTE: {
            const ComparisonMatchExpression* cmp =
                static_cast<const ComparisonMatchExpression*>( full );

            if ( cmp->getRHS().type() == jstNULL ) {
                // null and indexes don't play nice
                //spliceInfo->hasNullEquality = true;
                return NULL;
            }
        }
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

            // since { $in : [[1]] } matches [1], need to explode
            for ( BSONElementSet::const_iterator i = cloned->getArrayFilterEntries()->equalities().begin();
                  i != cloned->getArrayFilterEntries()->equalities().end();
                  ++i ) {
                const BSONElement& x = *i;
                if ( x.type() == Array ) {
                    BSONObjIterator j( x.Obj() );
                    while ( j.more() ) {
                        cloned->getArrayFilterEntries()->addEquality( j.next() );
                    }
                }
            }

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
