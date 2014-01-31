// expression_parser_text.cpp

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

#include "mongo/base/init.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression_text.h"

namespace mongo {

    StatusWithMatchExpression expressionParserTextCallbackReal( const BSONObj& queryObj ) {
        // Validate queryObj, but defer construction of FTSQuery (which requires access to the
        // target namespace) until stage building time.

        if ( mongo::String != queryObj["$search"].type() ) {
            return StatusWithMatchExpression( ErrorCodes::BadValue, "$search needs a String" );
        }

        string language = "";
        BSONElement languageElt = queryObj["$language"];
        if ( !languageElt.eoo() ) {
            if ( mongo::String != languageElt.type() ) {
                return StatusWithMatchExpression( ErrorCodes::BadValue,
                                                  "$language needs a String" );
            }
            language = languageElt.String();
            Status status =
                fts::FTSLanguage::make( language, fts::TEXT_INDEX_VERSION_2 ).getStatus();
            if ( !status.isOK() ) {
                return StatusWithMatchExpression( ErrorCodes::BadValue,
                                                  "$language specifies unsupported language" );
            }
        }
        string query = queryObj["$search"].String();

        if ( queryObj.nFields() != ( languageElt.eoo() ? 1 : 2 ) ) {
            return StatusWithMatchExpression( ErrorCodes::BadValue, "extra fields in $text" );
        }

        auto_ptr<TextMatchExpression> e( new TextMatchExpression() );
        Status s = e->init( query, language );
        if ( !s.isOK() ) {
            return StatusWithMatchExpression( s );
        }
        return StatusWithMatchExpression( e.release() );
    }

    MONGO_INITIALIZER( MatchExpressionParserText )( ::mongo::InitializerContext* context ) {
        expressionParserTextCallback = expressionParserTextCallbackReal;
        return Status::OK();
    }

}
