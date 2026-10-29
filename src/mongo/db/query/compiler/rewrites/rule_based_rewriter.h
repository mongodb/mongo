// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/checked_cast.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <boost/container/small_vector.hpp>

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
     * Called immediately after a rule transformation is applied.
     */
    virtual void postTransform() {}

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
     * Similar to std::priority_queue but does a linear search on pop() instead of maintaining a
     * heap. This is faster given that only a handful of rules are ever queued at the same time.
     */
    class RuleQueue {
    public:
        const Rule<Context>* pop() {
            tassert(11695601, "Can't pop from empty queue", !_rules.empty());

            auto it = std::max_element(_rules.begin(), _rules.end(), compareRules);
            auto* rule = *it;
            std::swap(*it, _rules.back());
            _rules.pop_back();
            return rule;
        }

        void push(const Rule<Context>* rule) {
            _rules.push_back(rule);
        }

        bool empty() const {
            return _rules.empty();
        }

        void clear() {
            _rules.clear();
        }

        size_t size() const {
            return _rules.size();
        }

    private:
        // We typically don't expect more than this many rules to be queued at the same time.
        static constexpr size_t kFewRules = 8;

        static bool compareRules(const Rule<Context>* a, const Rule<Context>* b) {
            return a->priority < b->priority;
        }

        boost::container::small_vector<const Rule<Context>*, kFewRules> _rules;
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

            // Must be acquired before precondition check, because precondition check can modify the
            // number of rules.
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
            _context.postTransform();

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
