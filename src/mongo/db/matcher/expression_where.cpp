// expression_where.cpp

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
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/scripting/engine.h"

namespace mongo {

    class WhereMatchExpression : public MatchExpression {
    public:
        WhereMatchExpression() : MatchExpression( WHERE ){ _func = 0; }
        virtual ~WhereMatchExpression(){}

        Status init( const StringData& ns, const StringData& theCode, const BSONObj& scope );

        virtual bool matches( const MatchableDocument* doc, MatchDetails* details = 0 ) const;

        virtual bool matchesSingleElement( const BSONElement& e ) const {
            return false;
        }

        virtual MatchExpression* shallowClone() const {
            WhereMatchExpression* e = new WhereMatchExpression();
            e->init(_ns, _code, _userScope);
            if ( getTag() ) {
                e->setTag(getTag()->clone());
            }
            return e;
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual bool equivalent( const MatchExpression* other ) const ;

        virtual void resetTag() { setTag(NULL); }

    private:
        string _ns;
        string _code;
        BSONObj _userScope;

        auto_ptr<Scope> _scope;
        ScriptingFunction _func;
    };

    Status WhereMatchExpression::init( const StringData& ns,
                                       const StringData& theCode,
                                       const BSONObj& scope ) {

        if ( ns.size() == 0 )
            return Status( ErrorCodes::BadValue, "ns for $where cannot be empty" );

        if ( theCode.size() == 0 )
            return Status( ErrorCodes::BadValue, "code for $where cannot be empty" );

        _ns = ns.toString();
        _code = theCode.toString();
        _userScope = scope.getOwned();

        NamespaceString nswrapper( _ns );
        const string userToken = ClientBasic::getCurrent()->getAuthorizationSession()
                                                          ->getAuthenticatedUserNamesToken();
        _scope = globalScriptEngine->getPooledScope( nswrapper.db().toString(),
                                                     "where" + userToken );
        _func = _scope->createFunction( _code.c_str() );

        if ( !_func )
            return Status( ErrorCodes::BadValue, "$where compile error" );

        return Status::OK();
    }

    bool WhereMatchExpression::matches( const MatchableDocument* doc, MatchDetails* details ) const {
        verify( _func );
        BSONObj obj = doc->toBSON();

        if ( ! _userScope.isEmpty() ) {
            _scope->init( &_userScope );
        }
        _scope->setObject( "obj", const_cast< BSONObj & >( obj ) );
        _scope->setBoolean( "fullObject" , true ); // this is a hack b/c fullObject used to be relevant

        int err = _scope->invoke( _func, 0, &obj, 1000 * 60, false );
        if ( err == -3 ) { // INVOKE_ERROR
            stringstream ss;
            ss << "error on invocation of $where function:\n"
               << _scope->getError();
            uassert( 16812, ss.str(), false);
        }
        else if ( err != 0 ) {   // ! INVOKE_SUCCESS
            uassert( 16813, "unknown error in invocation of $where function", false);
        }

        return _scope->getBoolean( "__returnValue" ) != 0;
    }

    void WhereMatchExpression::debugString( StringBuilder& debug, int level ) const {
        _debugAddSpace( debug, level );
        debug << "$where\n";

        _debugAddSpace( debug, level + 1 );
        debug << "ns: " << _ns << "\n";

        _debugAddSpace( debug, level + 1 );
        debug << "code: " << _code << "\n";

        _debugAddSpace( debug, level + 1 );
        debug << "scope: " << _userScope << "\n";
    }

    bool WhereMatchExpression::equivalent( const MatchExpression* other ) const {
        if ( matchType() != other->matchType() )
            return false;
        const WhereMatchExpression* realOther = static_cast<const WhereMatchExpression*>(other);
        return
            _ns == realOther->_ns &&
            _code == realOther->_code &&
            _userScope == realOther->_userScope;
    }


    // -----------------

    StatusWithMatchExpression expressionParserWhereCallbackReal(const BSONElement& where) {
        if ( !haveClient() )
            return StatusWithMatchExpression( ErrorCodes::BadValue, "no current client needed for $where" );

        Client::Context* context = cc().getContext();
        if ( !context )
            return StatusWithMatchExpression( ErrorCodes::BadValue, "no context in $where parsing" );

        const char* ns = context->ns();
        if ( !ns )
            return StatusWithMatchExpression( ErrorCodes::BadValue, "no ns in $where parsing" );

        if ( !globalScriptEngine )
            return StatusWithMatchExpression( ErrorCodes::BadValue, "no globalScriptEngine in $where parsing" );

        auto_ptr<WhereMatchExpression> exp( new WhereMatchExpression() );
        if ( where.type() == String || where.type() == Code ) {
            Status s = exp->init( ns, where.valuestr(), BSONObj() );
            if ( !s.isOK() )
                return StatusWithMatchExpression( s );
            return StatusWithMatchExpression( exp.release() );
        }

        if ( where.type() == CodeWScope ) {
            Status s = exp->init( ns,
                                  where.codeWScopeCode(),
                                  BSONObj( where.codeWScopeScopeDataUnsafe() ) );
            if ( !s.isOK() )
                return StatusWithMatchExpression( s );
            return StatusWithMatchExpression( exp.release() );
        }

        return StatusWithMatchExpression( ErrorCodes::BadValue, "$where got bad type" );
    }

    MONGO_INITIALIZER( MatchExpressionWhere )( ::mongo::InitializerContext* context ) {
        expressionParserWhereCallback = expressionParserWhereCallbackReal;
        return Status::OK();
    }




}
