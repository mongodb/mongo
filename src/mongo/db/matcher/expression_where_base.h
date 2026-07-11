// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Common base class for $where match expression implementations.
 */
class WhereMatchExpressionBase : public MatchExpression {
public:
    struct WhereParams {
        std::string code;
    };

    explicit WhereMatchExpressionBase(WhereParams params);

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400211);
    }

    void resetChild(size_t, MatchExpression*) override {
        MONGO_UNREACHABLE;
    };


    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final;

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    void setInputParamId(boost::optional<InputParamId> paramId) {
        _inputParamId = paramId;
    }

    boost::optional<InputParamId> getInputParamId() const {
        return _inputParamId;
    }

    const std::string& getCode() const {
        return _code;
    }

    virtual bool runPredicate(const BSONObj& doc) const = 0;

    // Out-of-line virtual dispatch helper: calls runPredicate() through a WhereMatchExpressionBase*
    // in a separate TU so the compiler cannot devirtualize it at call sites that know the concrete
    // derived type (e.g. WhereMatchExpression). Callers in query_expressions use this instead of
    // calling runPredicate() directly on a derived pointer.
    static bool evaluateWherePredicate(const WhereMatchExpressionBase* expr, const BSONObj& doc);

private:
    const std::string _code;

    boost::optional<InputParamId> _inputParamId;
};

}  // namespace mongo
