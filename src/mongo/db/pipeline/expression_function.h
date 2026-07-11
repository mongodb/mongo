// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <string>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;
/**
 * This expression takes a function, an array of arguments to pass to it, and the language
 * specifier (currently limited to JavaScript). It returns the return value of the function with
 * the given arguments.
 */
class ExpressionFunction final : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    static boost::intrusive_ptr<ExpressionFunction> create(
        ExpressionContext* const expCtx,
        boost::intrusive_ptr<Expression> passedArgs,
        std::string funcSourceString,
        std::string lang) {
        return new ExpressionFunction{expCtx,
                                      passedArgs,
                                      false /* don't assign first argument to 'this' */,
                                      std::move(funcSourceString),
                                      std::move(lang)};
    }

    // This method is intended for use when you want to bind obj to an argument for desugaring
    // $where.
    static boost::intrusive_ptr<ExpressionFunction> createForWhere(
        ExpressionContext* const expCtx,
        boost::intrusive_ptr<Expression> passedArgs,
        std::string funcSourceString,
        std::string lang) {
        return new ExpressionFunction{
            expCtx, passedArgs, true, std::move(funcSourceString), std::move(lang)};
    }

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    Value serialize(const query_shape::SerializationOptions& options) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const Expression* getPassedArgs() const {
        return _passedArgs.get();
    }

    bool getAssignFirstArgToThis() const {
        return _assignFirstArgToThis;
    }

    const std::string& getFuncSource() const {
        return _funcSource;
    }

    static constexpr auto kExpressionName = "$function"sv;
    static constexpr auto kJavaScript = "js";

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return ExpressionFunction::create(&expCtx, cloneChild(0, expCtx), _funcSource, _lang);
    }

private:
    ExpressionFunction(ExpressionContext* expCtx,
                       boost::intrusive_ptr<Expression> passedArgs,
                       bool assignFirstArgToThis,
                       std::string funcSourceString,
                       std::string lang);

    const boost::intrusive_ptr<Expression>& _passedArgs;
    bool _assignFirstArgToThis;
    std::string _funcSource;
    std::string _lang;

    template <typename H>
    friend class ExpressionHashVisitor;
};
}  // namespace mongo
