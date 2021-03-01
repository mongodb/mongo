/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

/**
 * A WindowFunctionState is a mutable, removable accumulator.
 *
 * Implementations must ensure that 'remove()' undoes 'add()' when called in FIFO order.
 * For example:
 *     'add(x); add(y); remove(x)' == 'add(y)'
 *     'add(a); add(b); add(z); remove(a); remove(b)' == 'add(z)'
 */
class WindowFunctionState {
public:
    WindowFunctionState(ExpressionContext* const expCtx) : _expCtx(expCtx) {}
    virtual ~WindowFunctionState() = default;
    virtual void add(Value) = 0;
    virtual void remove(Value) = 0;
    virtual Value getValue() const = 0;
    virtual void reset() = 0;

protected:
    ExpressionContext* _expCtx;
};


template <AccumulatorMinMax::Sense sense>
class WindowFunctionMinMax : public WindowFunctionState {
public:
    static inline const Value kDefault = Value{BSONNULL};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionMinMax<sense>>(expCtx);
    }

    explicit WindowFunctionMinMax(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx),
          _values(_expCtx->getValueComparator().makeOrderedValueMultiset()) {}

    void add(Value value) final {
        _values.insert(std::move(value));
    }

    void remove(Value value) final {
        // std::multiset::insert is guaranteed to put the element after any equal elements
        // already in the container. So find() / erase() will remove the oldest equal element,
        // which is what we want, to satisfy "remove() undoes add() when called in FIFO order".
        auto iter = _values.find(std::move(value));
        tassert(5371400, "Can't remove from an empty WindowFunctionMinMax", iter != _values.end());
        _values.erase(iter);
    }

    void reset() final {
        _values.clear();
    }

    Value getValue() const final {
        if (_values.empty())
            return kDefault;
        switch (sense) {
            case AccumulatorMinMax::Sense::kMin:
                return *_values.begin();
            case AccumulatorMinMax::Sense::kMax:
                return *_values.rbegin();
        }
        MONGO_UNREACHABLE_TASSERT(5371401);
    }

protected:
    // Holds all the values in the window, in order, with constant-time access to both ends.
    ValueMultiset _values;
};
using WindowFunctionMin = WindowFunctionMinMax<AccumulatorMinMax::Sense::kMin>;
using WindowFunctionMax = WindowFunctionMinMax<AccumulatorMinMax::Sense::kMax>;

class WindowFunctionAddToSet final : public WindowFunctionState {
public:
    static inline const Value kDefault = Value{std::vector<Value>()};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionAddToSet>(expCtx);
    }

    explicit WindowFunctionAddToSet(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx),
          _values(_expCtx->getValueComparator().makeOrderedValueMultiset()) {}

    void add(Value value) override {
        _values.insert(std::move(value));
    }

    /**
     * This should only remove the first/lowest element in the window.
     */
    void remove(Value value) override {
        auto iter = _values.find(std::move(value));
        tassert(
            5423800, "Can't remove from an empty WindowFunctionAddToSet", iter != _values.end());
        _values.erase(iter);
    }

    void reset() override {
        _values.clear();
    }

    Value getValue() const override {
        std::vector<Value> output;
        if (_values.empty())
            return kDefault;
        for (auto it = _values.begin(); it != _values.end(); it = _values.upper_bound(*it)) {
            output.push_back(*it);
        }

        return Value(output);
    }

private:
    ValueMultiset _values;
};

class WindowFunctionPush final : public WindowFunctionState {
public:
    using ValueListConstIterator = std::list<Value>::const_iterator;

    static inline const Value kDefault = Value{std::vector<Value>()};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionPush>(expCtx);
    }

    explicit WindowFunctionPush(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx),
          _values(
              _expCtx->getValueComparator().makeOrderedValueMultimap<ValueListConstIterator>()) {}

    void add(Value value) override {
        _list.emplace_back(std::move(value));
        auto iter = std::prev(_list.end());
        _values.insert({*iter, iter});
    }

    /**
     * This should only remove the first/lowest element in the window.
     */
    void remove(Value value) override {
        // The order of the key-value pairs whose keys compare equivalent is the order of insertion
        // and does not change in std::multimap. So find() / erase() will remove the oldest equal
        // element, which is what we want, to satisfy "remove() undoes add() when called in FIFO
        // order".
        auto iter = _values.find(std::move(value));
        tassert(5423801, "Can't remove from an empty WindowFunctionPush", iter != _values.end());
        // Erase the element from both '_values' and '_list'.
        _list.erase(iter->second);
        _values.erase(iter);
    }

    void reset() override {
        _values.clear();
        _list.clear();
    }

    Value getValue() const override {
        std::vector<Value> output;
        if (_values.empty())
            return kDefault;

        return Value{std::vector<Value>(_list.begin(), _list.end())};
    }

private:
    ValueMultimap<ValueListConstIterator> _values;
    // std::list makes sure that the order of the elements in the returned array is the order of
    // insertion.
    std::list<Value> _list;
};
}  // namespace mongo
