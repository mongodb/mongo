/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/pipeline/accumulation_statement.h"

#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

using boost::intrusive_ptr;
using std::string;

namespace {
// Used to keep track of which Accumulators are registered under which name.
using ParserRegistration =
    std::pair<AccumulationStatement::Parser,
              boost::optional<ServerGlobalParams::FeatureCompatibility::Version>>;
static StringMap<ParserRegistration> parserMap;
}  // namespace

void AccumulationStatement::registerAccumulator(
    std::string name,
    AccumulationStatement::Parser parser,
    boost::optional<ServerGlobalParams::FeatureCompatibility::Version> requiredMinVersion) {
    auto it = parserMap.find(name);
    massert(28722,
            str::stream() << "Duplicate accumulator (" << name << ") registered.",
            it == parserMap.end());
    parserMap[name] = {parser, requiredMinVersion};
}

AccumulationStatement::Parser& AccumulationStatement::getParser(
    StringData name,
    boost::optional<ServerGlobalParams::FeatureCompatibility::Version> allowedMaxVersion) {
    auto it = parserMap.find(name);
    uassert(
        15952, str::stream() << "unknown group operator '" << name << "'", it != parserMap.end());
    auto& [parser, requiredMinVersion] = it->second;
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            // We would like to include the current version and the required minimum version in this
            // error message, but using FeatureCompatibilityVersion::toString() would introduce a
            // dependency cycle (see SERVER-31968).
            str::stream() << name
                          << " is not allowed in the current feature compatibility version. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << " for more information.",
            !requiredMinVersion || !allowedMaxVersion || *requiredMinVersion <= *allowedMaxVersion);
    return parser;
}

boost::intrusive_ptr<AccumulatorState> AccumulationStatement::makeAccumulator() const {
    return expr.factory();
}

AccumulationStatement AccumulationStatement::parseAccumulationStatement(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONElement& elem,
    const VariablesParseState& vps) {
    auto fieldName = elem.fieldNameStringData();
    uassert(40234,
            str::stream() << "The field '" << fieldName << "' must be an accumulator object",
            elem.type() == BSONType::Object &&
                elem.embeddedObject().firstElementFieldName()[0] == '$');

    uassert(40235,
            str::stream() << "The field name '" << fieldName << "' cannot contain '.'",
            fieldName.find('.') == string::npos);

    uassert(40236,
            str::stream() << "The field name '" << fieldName << "' cannot be an operator name",
            fieldName[0] != '$');

    uassert(40238,
            str::stream() << "The field '" << fieldName << "' must specify one accumulator",
            elem.Obj().nFields() == 1);

    auto specElem = elem.Obj().firstElement();
    auto accName = specElem.fieldNameStringData();
    uassert(40237,
            str::stream() << "The " << accName << " accumulator is a unary operator",
            specElem.type() != BSONType::Array);

    auto&& parser =
        AccumulationStatement::getParser(accName, expCtx->maxFeatureCompatibilityVersion);
    auto [initializer, argument, factory] = parser(expCtx, specElem, vps);

    return AccumulationStatement(fieldName.toString(),
                                 AccumulationExpression(initializer, argument, factory));
}

}  // namespace mongo
