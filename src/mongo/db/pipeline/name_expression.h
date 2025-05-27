/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression.h"

#include <string>

#include <boost/intrusive_ptr.hpp>

namespace mongo {
class BSONObjBuilder;
class BSONElement;

/**
 * This class represents a name expression in the aggregation pipeline. It can be either a string
 * literal or a pipeline expression that evaluates to a string in the context of input document. For
 * example,
 *
 * {db: "test"} is a string literal.
 * {db: "$customer"} is a pipeline expression.
 * {coll: {$concat: ["$customer.department", "_", "$customer.name"]}} is a pipeline expression.
 *
 * The name expression is considered a string literal only when the value is a string and does not
 * start with '$' sign which is a path expression.
 *
 * This class can be used to represent the name of a collection or a database in the aggregation
 * pipeline when the name is not known until runtime but it can also be used to represent any entity
 * name in the pipeline that may be known at runtime.
 */
class NameExpression {
public:
    NameExpression() = default;
    NameExpression(const std::string& name) : NameExpression(BSON("" << name).firstElement()) {};
    NameExpression(const BSONElement& nameElem);
    NameExpression(const NameExpression&) = default;
    NameExpression(NameExpression&& other)
        : _name(std::move(other._name)),
          _isLiteral(other._isLiteral),
          _expr(std::move(other._expr)) {}

    NameExpression& operator=(const NameExpression&) = default;
    NameExpression& operator=(NameExpression&& other) {
        _name = std::move(other._name);
        _isLiteral = other._isLiteral;
        _expr = std::move(other._expr);
        return *this;
    }

    std::string toString() const;

    /**
     * Serializes into BSON object.
     */
    BSONObj toBSON() const;

    /*
     * These methods support IDL parsing of name expressions.
     */
    static NameExpression parseFromBSON(const BSONElement& elem);
    void serializeToBSON(StringData, BSONObjBuilder* bob) const;

    /**
     * Returns true if the name expression is a string literal.
     */
    bool isLiteral() const {
        return _isLiteral;
    }

    /**
     * Returns the string literal if the name expression is a string literal.
     */
    std::string getLiteral() const {
        tassert(
            8117103, fmt::format("Non string literal name expression: {}", toString()), _isLiteral);
        return _name.getElement().str();
    }

    /**
     * Evaluates the name expression in the context of the input document and returns the name as a
     * string.
     *
     * If the evaluation result is a non-string value, an exception is thrown. If the expression is
     * invalid, an exception is thrown.
     */
    std::string evaluate(ExpressionContext* expCtx, const Document& doc);

private:
    void compile(ExpressionContext* expCtx);

    // To serialize back to BSON.
    IDLAnyTypeOwned _name;

    bool _isLiteral = false;

    boost::intrusive_ptr<Expression> _expr = nullptr;
};

// To make the IDL compiler happy. Sometimes, it cannot find the serializeToBSON() method.
inline void serializeToBSON(const NameExpression& nameExpr, StringData, BSONObjBuilder* bob) {
    nameExpr.serializeToBSON(""_sd, bob);
}
}  // namespace mongo
