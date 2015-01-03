// expression_where_noop.cpp

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

#define MONGO_PCH_WHITELISTED
#include "mongo/platform/basic.h"
#include "mongo/pch.h"
#undef MONGO_PCH_WHITELISTED
#include "mongo/base/init.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"

namespace mongo {

    /**
     * Bogus no-op $where match expression to parse $where in mongos,
     * since mongos doesn't have script engine to compile JS functions.
     *
     * Linked into mongos, instead of the real WhereMatchExpression.
     */
    class WhereNoOpMatchExpression : public MatchExpression {
    public:
        WhereNoOpMatchExpression() : MatchExpression( WHERE ){ }
        virtual ~WhereNoOpMatchExpression(){}

        Status init( const StringData& theCode );

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const {
            return false;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const {
            return false;
        }

        virtual MatchExpression* shallowClone() const {
            WhereNoOpMatchExpression* e = new WhereNoOpMatchExpression();
            e->init(_code);
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual void toBSON(BSONObjBuilder* out) const;

        virtual bool equivalent( const MatchExpression* other ) const ;

        virtual void resetTag() { setTag(NULL); }

    private:
        string _code;
    };

    Status WhereNoOpMatchExpression::init(const StringData& theCode ) {
        if ( theCode.size() == 0 )
            return Status( ErrorCodes::BadValue, "code for $where cannot be empty" );

        _code = theCode.toString();

        return Status::OK();
    }

    void WhereNoOpMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$where (only in mongos)\n";

        _debugAddSpace( debug, level + 1 );
        debug << "code: " << _code << "\n";
    }

    void WhereNoOpMatchExpression::toBSON(BSONObjBuilder* out) const {
        out->append("$where", _code);
    }

    bool WhereNoOpMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;
        const WhereNoOpMatchExpression* noopOther = static_cast<const WhereNoOpMatchExpression*>(other);
        return _code == noopOther->_code;
    }


    // -----------------

    WhereCallbackNoop::WhereCallbackNoop() {

    }

    StatusWithMatchExpression WhereCallbackNoop::parseWhere(const BSONElement& where) const {

        auto_ptr<WhereNoOpMatchExpression> exp( new WhereNoOpMatchExpression() );
        if ( where.type() == String || where.type() == Code ) {
            Status s = exp->init( where.valuestr() );
            if ( !s.isOK() )
                return StatusWithMatchExpression( s );
            return StatusWithMatchExpression( exp.release() );
        }

        if ( where.type() == CodeWScope ) {
            Status s = exp->init( where.codeWScopeCode() );
            if ( !s.isOK() )
                return StatusWithMatchExpression( s );
            return StatusWithMatchExpression( exp.release() );
        }

        return StatusWithMatchExpression( ErrorCodes::BadValue, "$where got bad type" );
    }
}
