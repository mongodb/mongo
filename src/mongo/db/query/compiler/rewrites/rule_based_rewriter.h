/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/logv2/log.h"
#include "mongo/util/modules.h"

#include <functional>
#include <queue>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::rule_based_rewrites {

/**
 * Represents a rewrite rule, defined by a precondition, transformation and a priority.
 */
template <typename Context>
struct Rule {
    /**
     * Name of the rule. Must be unique. The convention is to use upper snake case.
     */
    std::string name;

    /**
     * Function that tells whether the rule can be applied.
     */
    std::function<bool(Context&)> precondition;

    /**
     * Function that allows us to apply the rule.
     *
     * Returns true when a transformation has done something like update/invalidate the current
     * position, or has changed the current position enough that we think more rules that have
     * been applied can be reapplied.
     */
    std::function<bool(Context&)> transform;

    /**
     * Priority of the rule. Higher value means higher priority.
     */
    double priority = 0;
};

template <typename Context>
std::partial_ordering operator<=>(const Rule<Context>& lhs, const Rule<Context>& rhs) {
    if (auto cmp = lhs.priority <=> rhs.priority; cmp != 0) {
        return cmp;
    }
    // Compare names to break tie. Rule names are assumed to be unique.
    return lhs.name <=> rhs.name;
}

template <typename Context>
class RewriteEngine;

/**
 * Interface for walking and modifying the structure (of type T) we are rewriting.
 */
template <typename SubClass, typename T>
class RewriteContext {
public:
    /**
     * Returns false if we are at the end of the structure we are rewriting, and true otherwise.
     */
    virtual bool hasMore() const = 0;

    /**
     * Advances to the next element in the structure we are rewriting.
     */
    virtual void advance() = 0;

    /**
     * The element we are currently rewriting.
     */
    virtual T& current() = 0;
    virtual const T& current() const = 0;

    /**
     * Enqueues rules to be applied based on the current element. Called from the engine after
     * advancing to the next element.
     */
    virtual void enqueueRules() = 0;

    /**
     * Enqueues given rules to be applied to the current element.
     */
    void addRules(std::vector<Rule<SubClass>> rules) {
        tassert(11010014, "Engine not initialized", _engine);
        _engine->addRules(std::move(rules));
    }

    /**
     * Helper to get the current element as its subtype.
     */
    template <std::derived_from<T> SubType>
    SubType& currentAs() {
        return static_cast<SubType&>(current());
    }
    template <std::derived_from<T> SubType>
    const SubType& currentAs() const {
        return static_cast<const SubType&>(current());
    }

    void setEngine(RewriteEngine<SubClass>& engine) {
        _engine = &engine;
    }

private:
    RewriteEngine<SubClass>* _engine;
};

/**
 * Concrete class responsible for driving the rewrite process. Provides an entry point for
 * optimization and manages a queue of rules to be applied. Owns the Context, which is used
 * to specialize the behavior of the engine.
 */
template <typename Context>
class RewriteEngine final {
public:
    RewriteEngine(Context context) : _context(std::move(context)) {
        _context.setEngine(*this);
    }

    void addRule(Rule<Context> rule) {
        _rules.emplace(std::move(rule));
    }

    void addRules(std::vector<Rule<Context>> rules) {
        for (auto&& rule : rules) {
            addRule(std::move(rule));
        }
    }

    /**
     * Entry point to optimization.
     */
    void applyRules() {
        while (_context.hasMore()) {
            // Enqueue rules for the current position. Note that transforms can queue additional
            // rules.
            _context.enqueueRules();

            bool doAdvance = true;
            while (!_rules.empty() && _context.hasMore()) {
                const auto rule = std::move(_rules.top());
                _rules.pop();
                const size_t rulesBefore = _rules.size();

                LOGV2_DEBUG(11010013,
                            5,
                            "Trying to apply a rewrite rule",
                            "rule"_attr = rule.name,
                            "priority"_attr = rule.priority);

                if (rule.precondition(_context)) {
                    const bool shouldRequeueRules = rule.transform(_context);
                    if (shouldRequeueRules) {
                        tassert(11010015,
                                "Should not add new rules from a rule that requires requeueing",
                                rulesBefore == _rules.size());

                        // Discard remaining rules because we changed position.
                        clearRules();
                        doAdvance = false;
                        break;
                    }
                }
            }

            if (doAdvance) {
                // Did not update position. Advance to the next element.
                _context.advance();
            }
        }
    }

private:
    void clearRules() {
        _rules = {};
    }

    Context _context;
    std::priority_queue<Rule<Context>> _rules;
};

}  // namespace mongo::rule_based_rewrites

#undef MONGO_LOGV2_DEFAULT_COMPONENT
