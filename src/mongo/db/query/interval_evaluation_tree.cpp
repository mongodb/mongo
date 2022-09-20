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

namespace mongo::interval_evaluation_tree {
namespace {
class Printer {
public:
    static constexpr char kOpen = '(';
    static constexpr char kClose = ')';

    Printer(std::ostream& os) : _os{os} {}

    void operator()(const IET&, const UnionNode& node) {
        _os << kOpen << "union ";
        node.get<0>().visit(*this);
        _os << ' ';
        node.get<1>().visit(*this);
        _os << kClose;
    }

    void operator()(const IET&, const IntersectNode& node) {
        _os << kOpen << "intersect ";
        node.get<0>().visit(*this);
        _os << ' ';
        node.get<1>().visit(*this);
        _os << kClose;
    }

    void operator()(const IET&, const ConstNode& node) {
        _os << kOpen << "const";
        for (auto&& interval : node.oil.intervals) {
            _os << ' ' << interval.toString(false);
        }
        _os << kClose;
    }

    void operator()(const IET&, const EvalNode& node) {
        _os << kOpen << "eval " << matchTypeToString(node.matchType()) << " #"
            << node.inputParamId() << kClose;
    }

    void operator()(const IET&, const ComplementNode& node) {
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
            case MatchExpression::TYPE_OPERATOR:
                return "$type";
            default:
                tasserted(6334800, str::stream() << "unexpected MatchType " << matchType);
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

/**
 * Evaluates given Interval Evalution Tree to index bounds represented by OrderedIntervalList.
 *
 * This class is intended to live for a short period only as it keeps references to some external
 * objects such as the index entry, BSONElement and inputParamIdMap and it is imperative that the
 * referenced objects stay alive for the lifetime of the class.
 */
class IntervalEvalTransporter {
public:
    IntervalEvalTransporter(const std::vector<const MatchExpression*>& inputParamIdMap,
                            const IndexEntry& index,
                            const BSONElement& elt)
        : _index{index}, _elt{elt}, _inputParamIdMap{inputParamIdMap} {}

    OrderedIntervalList transport(const IntersectNode&,
                                  OrderedIntervalList&& left,
                                  OrderedIntervalList&& right) const {
        IndexBoundsBuilder::intersectize(right, &left);
        return std::move(left);
    }

    OrderedIntervalList transport(const UnionNode&,
                                  OrderedIntervalList&& left,
                                  OrderedIntervalList&& right) const {
        for (auto&& interval : right.intervals) {
            left.intervals.emplace_back(std::move(interval));
        }

        IndexBoundsBuilder::unionize(&left);
        return std::move(left);
    }

    OrderedIntervalList transport(const ComplementNode&, OrderedIntervalList&& child) const {
        child.complement();
        return std::move(child);
    }

    OrderedIntervalList transport(const EvalNode& node) const {
        tassert(6335000,
                "InputParamId is not found",
                static_cast<size_t>(node.inputParamId()) < _inputParamIdMap.size());
        auto expr = _inputParamIdMap[node.inputParamId()];

        OrderedIntervalList oil{};
        IndexBoundsBuilder::translate(expr, _elt, _index, &oil);
        return oil;
    }

    OrderedIntervalList transport(const ConstNode& node) const {
        return node.oil;
    }

private:
    const IndexEntry& _index;
    const BSONElement& _elt;
    const std::vector<const MatchExpression*>& _inputParamIdMap;
};
}  // namespace

std::string ietToString(const IET& iet) {
    std::ostringstream oss;
    Printer printer{oss};
    iet.visit(printer);
    return oss.str();
}

std::string ietsToString(const IndexEntry& index, const std::vector<IET>& iets) {
    tassert(6334801,
            "IET vector must have the same number of elements as the key pattern",
            index.keyPattern.nFields() == static_cast<int>(iets.size()));

    std::ostringstream oss;
    Printer printer{oss};

    oss << Printer::kOpen << "iets " << index.keyPattern;

    BSONObjIterator it(index.keyPattern);
    for (const auto& iet : iets) {
        oss << ' ' << Printer::kOpen << it.next() << ' ';
        iet.visit(printer);
        oss << Printer::kClose;
    }

    oss << Printer::kClose;
    return oss.str();
}

void Builder::addIntersect() {
    tassert(6334802, "Intersection requires two index intervals", _intervals.size() >= 2);
    auto rhs = std::move(_intervals.top());
    _intervals.pop();
    auto lhs = std::move(_intervals.top());
    _intervals.pop();
    _intervals.push(makeInterval<IntersectNode>(std::move(lhs), std::move(rhs)));
}

void Builder::addUnion() {
    tassert(6334803, "Union requires two index intervals", _intervals.size() >= 2);
    auto rhs = std::move(_intervals.top());
    _intervals.pop();
    auto lhs = std::move(_intervals.top());
    _intervals.pop();
    _intervals.push(makeInterval<UnionNode>(std::move(lhs), std::move(rhs)));
}

void Builder::addComplement() {
    tassert(6334804, "Not requires at least one index interval", _intervals.size() >= 1);
    auto child = std::move(_intervals.top());
    _intervals.pop();
    _intervals.push(makeInterval<ComplementNode>(std::move(child)));
}

void Builder::addEval(const MatchExpression& expr, const OrderedIntervalList& oil) {
    auto inputParamId = [&expr]() -> boost::optional<MatchExpression::InputParamId> {
        if (ComparisonMatchExpression::isComparisonMatchExpression(&expr)) {
            return extractInputParamId<ComparisonMatchExpression>(&expr);
        }

        switch (expr.matchType()) {
            case MatchExpression::MATCH_IN:
                return extractInputParamId<InMatchExpression>(&expr);
            case MatchExpression::TYPE_OPERATOR:
                return extractInputParamId<TypeMatchExpression>(&expr);
            case MatchExpression::REGEX: {
                const auto* regexExpr = checked_cast<const RegexMatchExpression*>(&expr);
                const auto inputParamId = regexExpr->getSourceRegexInputParamId();
                tassert(6334805, "RegexMatchExpression must be parameterized", inputParamId);
                return inputParamId;
            }
            default:
                tasserted(6334806,
                          str::stream()
                              << "Got unexpected expression to translate: " << expr.matchType());
        };
    }();

    if (inputParamId) {
        _intervals.push(makeInterval<EvalNode>(*inputParamId, expr.matchType()));
    } else {
        addConst(oil);
    }
}

void Builder::addConst(const OrderedIntervalList& oil) {
    _intervals.push(makeInterval<ConstNode>(oil));
}

bool Builder::isEmpty() const {
    return _intervals.empty();
}

void Builder::pop() {
    tassert(6944101, "Intervals list is empty", !_intervals.empty());
    _intervals.pop();
}

boost::optional<IET> Builder::done() const {
    if (_intervals.empty()) {
        return boost::none;
    }

    tassert(6334807, "All intervals should be merged into one", _intervals.size() == 1);
    return _intervals.top();
}

OrderedIntervalList evaluateIntervals(const IET& iet,
                                      const std::vector<const MatchExpression*>& inputParamIdMap,
                                      const BSONElement& elt,
                                      const IndexEntry& index) {
    IntervalEvalTransporter transporter{inputParamIdMap, index, elt};
    return optimizer::algebra::transport(iet, transporter);
}
}  // namespace mongo::interval_evaluation_tree
