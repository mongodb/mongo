/**
 * Copyright (c) 2011 10gen Inc.
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
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <unordered_set>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/stdx/functional.h"

namespace mongo {
/**
 * Registers an Accumulator to have the name 'key'. When an accumulator with name '$key' is found
 * during parsing of a $group stage, 'factory' will be called to construct the Accumulator.
 *
 * As an example, if your accumulator looks like {"$foo": <args>}, with a factory method 'create',
 * you would add this line:
 * REGISTER_EXPRESSION(foo, AccumulatorFoo::create);
 */
#define REGISTER_ACCUMULATOR(key, factory)                                     \
    MONGO_INITIALIZER(addToAccumulatorFactoryMap_##key)(InitializerContext*) { \
        Accumulator::registerAccumulator("$" #key, (factory));                 \
        return Status::OK();                                                   \
    }

class Accumulator : public RefCountable {
public:
    using Factory = boost::intrusive_ptr<Accumulator>(*)();

    Accumulator() = default;

    /** Process input and update internal state.
     *  merging should be true when processing outputs from getValue(true).
     */
    void process(const Value& input, bool merging) {
        processInternal(input, merging);
    }

    /** Marks the end of the evaluate() phase and return accumulated result.
     *  toBeMerged should be true when the outputs will be merged by process().
     */
    virtual Value getValue(bool toBeMerged) const = 0;

    /// The name of the op as used in a serialization of the pipeline.
    virtual const char* getOpName() const = 0;

    int memUsageForSorter() const {
        dassert(_memUsageBytes != 0);  // This would mean subclass didn't set it
        return _memUsageBytes;
    }

    /// Reset this accumulator to a fresh state ready to receive input.
    virtual void reset() = 0;

    /**
     * Registers an Accumulator with a parsing function, so that when an accumulator with the given
     * name is encountered during parsing of the $group stage, it will call 'factory' to construct
     * that Accumulator.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_ACCUMULATOR macro defined in this
     * file.
     */
    static void registerAccumulator(std::string name, Factory factory);

    /**
     * Retrieves the Factory for the accumulator specified by the given name, and raises an error if
     * there is no such Accumulator registered.
     */
    static Factory getFactory(StringData name);

    virtual bool isAssociative() const {
        return false;
    }

    virtual bool isCommutative() const {
        return false;
    }

protected:
    /// Update subclass's internal state based on input
    virtual void processInternal(const Value& input, bool merging) = 0;

    /// subclasses are expected to update this as necessary
    int _memUsageBytes = 0;
};


class AccumulatorAddToSet final : public Accumulator {
public:
    AccumulatorAddToSet();

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) const final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<Accumulator> create();

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

private:
    typedef std::unordered_set<Value, Value::Hash> SetType;
    SetType set;
};


class AccumulatorFirst final : public Accumulator {
public:
    AccumulatorFirst();

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) const final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<Accumulator> create();

private:
    bool _haveFirst;
    Value _first;
};


class AccumulatorLast final : public Accumulator {
public:
    AccumulatorLast();

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) const final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<Accumulator> create();

private:
    Value _last;
};


class AccumulatorSum final : public Accumulator {
public:
    AccumulatorSum();

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) const final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<Accumulator> create();

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

private:
    BSONType totalType;
    long long longTotal;
    double doubleTotal;
};


class AccumulatorMinMax : public Accumulator {
public:
    enum Sense : int {
        MIN = 1,
        MAX = -1,  // Used to "scale" comparison.
    };

    explicit AccumulatorMinMax(Sense sense);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) const final;
    const char* getOpName() const final;
    void reset() final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

private:
    Value _val;
    const Sense _sense;
};

class AccumulatorMax final : public AccumulatorMinMax {
public:
    AccumulatorMax() : AccumulatorMinMax(MAX) {}
    static boost::intrusive_ptr<Accumulator> create();
};

class AccumulatorMin final : public AccumulatorMinMax {
public:
    AccumulatorMin() : AccumulatorMinMax(MIN) {}
    static boost::intrusive_ptr<Accumulator> create();
};


class AccumulatorPush final : public Accumulator {
public:
    AccumulatorPush();

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) const final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<Accumulator> create();

private:
    std::vector<Value> vpValue;
};


class AccumulatorAvg final : public Accumulator {
public:
    AccumulatorAvg();

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) const final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<Accumulator> create();

private:
    double _total;
    long long _count;
};


class AccumulatorStdDev : public Accumulator {
public:
    explicit AccumulatorStdDev(bool isSamp);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) const final;
    const char* getOpName() const final;
    void reset() final;

private:
    const bool _isSamp;
    long long _count;
    double _mean;
    double _m2;  // Running sum of squares of delta from mean. Named to match algorithm.
};

class AccumulatorStdDevPop final : public AccumulatorStdDev {
public:
    AccumulatorStdDevPop() : AccumulatorStdDev(false) {}
    static boost::intrusive_ptr<Accumulator> create();
};

class AccumulatorStdDevSamp final : public AccumulatorStdDev {
public:
    AccumulatorStdDevSamp() : AccumulatorStdDev(true) {}
    static boost::intrusive_ptr<Accumulator> create();
};
}
