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

#include "mongo/base/checked_cast.h"
#include "mongo/logv2/log.h"
#include "mongo/util/modules.h"

#include <functional>
#include <queue>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::rule_based_rewrites {

// Represents a set of tags that can be assigned to a rule. It is up to implementations of `Context`
// to define the meaning of each bit/tag.
using TagSet = uint32_t;

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

    /**
     * The engine can be made to only apply rules that have been assigned a certain tag.
     */
    TagSet tags = 0;
};

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
    void addRules(const std::vector<Rule<SubClass>>& rules) {
        for (auto&& rule : rules) {
            addRule(rule);
        }
    }
    void addRule(const Rule<SubClass>& rule) {
        tassert(11010014, "Engine not initialized", _engine);
        _engine->addRule(rule);
    }

    /**
     * Helper to get the current element as its subtype.
     */
    template <std::derived_from<T> SubType>
    SubType& currentAs() {
        return checked_cast<SubType&>(current());
    }
    template <std::derived_from<T> SubType>
    const SubType& currentAs() const {
        return checked_cast<const SubType&>(current());
    }

    void setEngine(RewriteEngine<SubClass>& engine) {
        _engine = &engine;
    }

private:
    RewriteEngine<SubClass>* _engine;
};

namespace rule_detail {
// Whether this rule has been assigned least one of the given tags.
template <typename Context>
constexpr bool hasTag(const Rule<Context>& rule, TagSet tags) {
    if (tags == 0) {
        // Empty tag set denotes that we want to run all rules.
        return true;
    }
    return tags & rule.tags;
}
}  // namespace rule_detail

/**
 * Concrete class responsible for driving the rewrite process. Provides an entry point for
 * optimization and manages a queue of rules to be applied. Owns the Context, which is used
 * to specialize the behavior of the engine.
 */
template <typename Context>
class RewriteEngine final {
public:
    RewriteEngine(Context context, size_t maxRewrites = std::numeric_limits<size_t>::max())
        : _context(std::move(context)), _maxRewrites(maxRewrites) {
        _context.setEngine(*this);
    }

    void addRule(const Rule<Context>& rule) {
        if (rule_detail::hasTag(rule, _tagsToRun)) {
            _rules.push(&rule);
        }
    }

    /**
     * Entry point to optimization.
     */
    void applyRules(TagSet tagsToRun = 0) {
        _tagsToRun = tagsToRun;

        while (_context.hasMore()) {
            // Enqueue rules for the current position. Note that transforms can queue additional
            // rules.
            _context.enqueueRules();

            NextAction nextAction = rewriteCurrentPosition();
            switch (nextAction) {
                case NextAction::Requeue:
                    // Requeue rules that apply to the current element without advancing.
                    clearRules();
                    break;
                case NextAction::Advance:
                    // Did not update position. Advance to the next element.
                    _context.advance();
                    break;
                case NextAction::Bail:
                    return;
            }
        }
    }

private:
    /**
     * Similar to std::priority_queue, but allows us clear() the queue without changing capacity,
     * which helps performance when rules are being requeued over and over again.
     */
    class RuleQueue {
    public:
        const Rule<Context>* pop() {
            std::pop_heap(_queue.begin(), _queue.end(), _compare);
            auto& result = _queue.back();
            _queue.pop_back();
            return result;
        }

        void push(const Rule<Context>* rule) {
            _queue.push_back(rule);
            std::push_heap(_queue.begin(), _queue.end(), _compare);
        }

        bool empty() const {
            return _queue.empty();
        }

        void clear() {
            _queue.clear();
        }

        size_t size() const {
            return _queue.size();
        }

    private:
        struct HighestPriorityFirst {
            bool operator()(const Rule<Context>* a, const Rule<Context>* b) const {
                return a->priority < b->priority;
            }
        };

        HighestPriorityFirst _compare{};
        std::vector<const Rule<Context>*> _queue;
    };

    enum class NextAction {
        Requeue,
        Advance,
        Bail,
    };

    /**
     * Try to apply all rules in the queue to the current position.
     */
    NextAction rewriteCurrentPosition() {
        while (!_rules.empty() && _context.hasMore()) {
            if (_maxRewrites <= _rewritesApplied) {
                LOGV2_DEBUG(11020801,
                            5,
                            "Reached the maximum number of rewrites applied",
                            "limit"_attr = _maxRewrites);
                return NextAction::Bail;
            }

            const auto& rule = *_rules.pop();
            const size_t rulesBefore = _rules.size();

            LOGV2_DEBUG(11010013,
                        5,
                        "Trying to apply a rewrite rule",
                        "rule"_attr = rule.name,
                        "priority"_attr = rule.priority);

            if (!rule.precondition(_context)) {
                // Continue to the next applicable rule.
                continue;
            }

            const bool shouldRequeueRules = rule.transform(_context);
            _rewritesApplied++;

            LOGV2_DEBUG(11206202, 5, "Applied rule", "rule"_attr = rule.name);

            if (shouldRequeueRules) {
                tassert(11010015,
                        "Should not add new rules from a rule that requires requeueing",
                        rulesBefore == _rules.size());
                // Discard remaining rules and requeue because we changed position.
                return NextAction::Requeue;
            }
        }

        return NextAction::Advance;
    }

    void clearRules() {
        _rules.clear();
    }

    Context _context;
    RuleQueue _rules;
    // Only rules with at least one of these tags will be applied.
    TagSet _tagsToRun{0};

    const size_t _maxRewrites;
    size_t _rewritesApplied{0};
};

}  // namespace mongo::rule_based_rewrites

#undef MONGO_LOGV2_DEFAULT_COMPONENT
