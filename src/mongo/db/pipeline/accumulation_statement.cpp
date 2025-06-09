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

#include "mongo/db/pipeline/accumulation_statement.h"

#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <string>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using std::string;

namespace {
// Used to keep track of which Accumulators are registered under which name and in which contexts
// they can be used in.
static StringMap<AccumulationStatement::ParserRegistration> parserMap;
}  // namespace

void AccumulationStatement::registerAccumulator(std::string name,
                                                AccumulationStatement::Parser parser,
                                                AllowedWithApiStrict allowedWithApiStrict,
                                                AllowedWithClientType allowedWithClientType,
                                                FeatureFlag* featureFlag) {
    auto it = parserMap.find(name);
    massert(28722,
            str::stream() << "Duplicate accumulator (" << name << ") registered.",
            it == parserMap.end());
    parserMap[name] = {parser, allowedWithApiStrict, allowedWithClientType, featureFlag};
    operatorCountersGroupAccumulatorExpressions.addCounter(name);
}

AccumulationStatement::ParserRegistration& AccumulationStatement::getParser(StringData name) {
    auto it = parserMap.find(name);
    uassert(
        15952, str::stream() << "unknown group operator '" << name << "'", it != parserMap.end());
    return it->second;
}

boost::intrusive_ptr<AccumulatorState> AccumulationStatement::makeAccumulator() const {
    return expr.factory();
}

AccumulationStatement AccumulationStatement::parseAccumulationStatement(
    ExpressionContext* const expCtx, const BSONElement& elem, const VariablesParseState& vps) {
    auto fieldName = elem.fieldNameStringData();
    uassert(40234,
            str::stream() << "The field '" << fieldName << "' must be an accumulator object",
            elem.type() == BSONType::object &&
                elem.embeddedObject().firstElementFieldName()[0] == '$');

    uassert(40235,
            str::stream() << "The field name '" << fieldName << "' cannot contain '.'",
            fieldName.find('.') == string::npos);

    uassert(40236,
            str::stream() << "The field name '" << fieldName << "' cannot be an operator name",
            fieldName[0] != '$');

    uassert(40238,
            str::stream() << "The field '" << fieldName
                          << "' must specify one accumulator: " << elem,
            elem.Obj().nFields() == 1);

    auto specElem = elem.Obj().firstElement();
    auto accName = specElem.fieldNameStringData();
    uassert(40237,
            str::stream() << "The " << accName << " accumulator is a unary operator",
            specElem.type() != BSONType::array);

    auto&& [parser, allowedWithApiStrict, allowedWithClientType, featureFlag] =
        AccumulationStatement::getParser(accName);

    if (featureFlag) {
        expCtx->ignoreFeatureInParserOrRejectAndThrow(accName, *featureFlag);
    }

    tassert(5837900,
            "Accumulators should only appear in a user operation",
            expCtx->getOperationContext());
    assertLanguageFeatureIsAllowed(expCtx->getOperationContext(),
                                   std::string{accName},
                                   allowedWithApiStrict,
                                   allowedWithClientType);

    expCtx->incrementGroupAccumulatorExprCounter(accName);
    auto accExpr = parser(expCtx, specElem, vps);

    return AccumulationStatement(std::string{fieldName}, std::move(accExpr));
}

MONGO_INITIALIZER_GROUP(BeginAccumulatorRegistration, ("default"), ("EndAccumulatorRegistration"))
MONGO_INITIALIZER_GROUP(EndAccumulatorRegistration, ("BeginAccumulatorRegistration"), ())
}  // namespace mongo
