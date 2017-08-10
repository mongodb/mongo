/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <boost/intrusive_ptr.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

/**
 * Registers an Accumulator to have the name 'key'. When an accumulator with name '$key' is found
 * during parsing, 'factory' will be called to construct the Accumulator.
 *
 * As an example, if your accumulator looks like {"$foo": <args>}, with a factory method 'create',
 * you would add this line:
 * REGISTER_ACCUMULATOR(foo, AccumulatorFoo::create);
 */
#define REGISTER_ACCUMULATOR(key, factory)                                     \
    MONGO_INITIALIZER(addToAccumulatorFactoryMap_##key)(InitializerContext*) { \
        AccumulationStatement::registerAccumulator("$" #key, (factory));       \
        return Status::OK();                                                   \
    }

/**
 * A class representing a user-specified accummulation, including the field name to put the
 * accumulated result in, which accumulator to use, and the expression used to obtain the input to
 * the Accumulator.
 */
class AccumulationStatement {
public:
    AccumulationStatement(std::string fieldName,
                          boost::intrusive_ptr<Expression> expression,
                          Accumulator::Factory factory)
        : fieldName(std::move(fieldName)),
          expression(std::move(expression)),
          _factory(std::move(factory)) {}

    /**
     * Parses a BSONElement that is an accumulated field, and returns an AccumulationStatement for
     * that accumulated field.
     *
     * Throws a AssertionException if parsing fails.
     */
    static AccumulationStatement parseAccumulationStatement(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const BSONElement& elem,
        const VariablesParseState& vps);

    /**
     * Registers an Accumulator with a parsing function, so that when an accumulator with the given
     * name is encountered during parsing, we will know to call 'factory' to construct that
     * Accumulator.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_ACCUMULATOR macro defined in this
     * file.
     */
    static void registerAccumulator(std::string name, Accumulator::Factory factory);

    /**
     * Retrieves the Factory for the accumulator specified by the given name, and raises an error if
     * there is no such Accumulator registered.
     */
    static Accumulator::Factory getFactory(StringData name);

    // The field name is used to store the results of the accumulation in a result document.
    std::string fieldName;

    // The expression to use to obtain the input to the accumulator.
    boost::intrusive_ptr<Expression> expression;

    // Constructs an Accumulator to do actual accumulation.
    boost::intrusive_ptr<Accumulator> makeAccumulator(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const;

private:
    Accumulator::Factory _factory;
};

}  // namespace mongo
