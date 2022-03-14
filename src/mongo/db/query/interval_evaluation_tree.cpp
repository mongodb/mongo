/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/interval_evaluation_tree.h"

#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/query/index_bounds_builder.h"

namespace mongo {
namespace {
class IetPrinter {
public:
    static constexpr char kOpen = '(';
    static constexpr char kClose = ')';

    IetPrinter(std::ostream& os) : _os{os} {}

    void operator()(const IET&, const UnionInterval& node) {
        _os << kOpen << "union ";
        node.get<0>().visit(*this);
        _os << ' ';
        node.get<1>().visit(*this);
        _os << kClose;
    }

    void operator()(const IET&, const IntersectInterval& node) {
        _os << kOpen << "intersect ";
        node.get<0>().visit(*this);
        _os << ' ';
        node.get<1>().visit(*this);
        _os << kClose;
    }

    void operator()(const IET&, const ConstInterval& node) {
        _os << kOpen << "const";
        for (auto&& interval : node.intervals) {
            _os << ' ' << interval.toString(false);
        }
        _os << kClose;
    }

    void operator()(const IET&, const EvalInterval& node) {
        _os << kOpen << "eval " << matchTypeToString(node.matchType()) << " #"
            << node.inputParamId() << kClose;
    }

    void operator()(const IET&, const ComplementInterval& node) {
        _os << kOpen << "not ";
        node.get<0>().visit(*this);
        _os << kClose;
    }

private:
    static std::string matchTypeToString(const MatchExpression::MatchType& matchType) {
        switch (matchType) {
            case MatchExpression::EQ:
                return "$eq";
            case MatchExpression::LTE:
                return "$lte";
            case MatchExpression::LT:
                return "$lt";
            case MatchExpression::GTE:
                return "$gte";
            case MatchExpression::GT:
                return "$gt";
            case MatchExpression::MATCH_IN:
                return "$in";
            case MatchExpression::REGEX:
                return "$regex";
            default:
                tasserted(6334800, str::stream() << "unexpected MatchType" << matchType);
        }
    }

    std::ostream& _os;
};

template <typename T, typename... Args>
inline auto makeInterval(Args&&... args) {
    return IET::make<T>(std::forward<Args>(args)...);
}

template <typename T, typename = std::enable_if_t<std::is_convertible_v<T*, PathMatchExpression*>>>
auto extractInputParamId(const MatchExpression* expr) {
    return checked_cast<const T*>(expr)->getInputParamId();
}

}  // namespace

std::string ietToString(const IET& iet) {
    std::ostringstream oss;
    IetPrinter printer{oss};
    iet.visit(printer);
    return oss.str();
}

std::string ietsToString(const IndexEntry& index, const std::vector<IET>& iets) {
    tassert(6334801,
            "IET vector must have the same number of elements as the key pattern",
            index.keyPattern.nFields() == static_cast<int>(iets.size()));

    std::ostringstream oss;
    IetPrinter printer{oss};

    oss << IetPrinter::kOpen << "iets " << index.keyPattern;

    BSONObjIterator it(index.keyPattern);
    for (const auto& iet : iets) {
        oss << ' ' << IetPrinter::kOpen << it.next() << ' ';
        iet.visit(printer);
        oss << IetPrinter::kClose;
    }

    oss << IetPrinter::kClose;
    return oss.str();
}

void IETBuilder::intersectIntervals() {
    tassert(6334802, "Intersection requires two index intervals", _intervals.size() >= 2);
    auto rhs = std::move(_intervals.top());
    _intervals.pop();
    auto lhs = std::move(_intervals.top());
    _intervals.pop();
    _intervals.push(makeInterval<IntersectInterval>(std::move(lhs), std::move(rhs)));
}

void IETBuilder::unionIntervals() {
    tassert(6334803, "Union requires two index intervals", _intervals.size() >= 2);
    auto rhs = std::move(_intervals.top());
    _intervals.pop();
    auto lhs = std::move(_intervals.top());
    _intervals.pop();
    _intervals.push(makeInterval<UnionInterval>(std::move(lhs), std::move(rhs)));
}

void IETBuilder::complementInterval() {
    tassert(6334804, "Not requires at least one index interval", _intervals.size() >= 1);
    auto child = std::move(_intervals.top());
    _intervals.pop();
    _intervals.push(makeInterval<ComplementInterval>(std::move(child)));
}

void IETBuilder::translate(const MatchExpression& expr, const OrderedIntervalList& oil) {
    tassert(6334805, "OrderedIntervalList must be non empty", !oil.intervals.empty());

    auto [inputParamId, shouldAppendConstInterval] =
        [&expr]() -> std::pair<boost::optional<MatchExpression::InputParamId>, bool> {
        if (ComparisonMatchExpression::isComparisonMatchExpression(&expr)) {
            return {extractInputParamId<ComparisonMatchExpression>(&expr), true};
        }

        switch (expr.matchType()) {
            case MatchExpression::MATCH_IN:
                return {extractInputParamId<InMatchExpression>(&expr), true};
            case MatchExpression::EXISTS:
            case MatchExpression::MOD:
                return {boost::none, true};
            case MatchExpression::ELEM_MATCH_VALUE:
            case MatchExpression::NOT:
                return {boost::none, false};
            case MatchExpression::REGEX: {
                const auto* regexExpr = checked_cast<const RegexMatchExpression*>(&expr);
                const auto inputParamId = regexExpr->getSourceRegexInputParamId();
                tassert(6334810, "RegexMatchExpression must be parameterized", inputParamId);
                return {inputParamId, false};
            }
            default:
                tasserted(6334811,
                          str::stream()
                              << "Got unexpected expression to translate: " << expr.matchType());
        };
    }();

    if (inputParamId) {
        _intervals.push(makeInterval<EvalInterval>(*inputParamId, expr.matchType()));
    } else if (shouldAppendConstInterval) {
        _intervals.push(makeInterval<ConstInterval>(oil.intervals));
    }
}

void IETBuilder::appendIntervalList(const OrderedIntervalList& oil) {
    _intervals.push(makeInterval<ConstInterval>(oil.intervals));
}

boost::optional<IET> IETBuilder::done() const {
    if (_intervals.empty()) {
        return boost::none;
    }

    tassert(6334812, "All intervals should be merged into one", _intervals.size() == 1);
    return _intervals.top();
}
}  // namespace mongo
