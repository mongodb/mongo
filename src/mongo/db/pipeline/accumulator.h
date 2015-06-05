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
#include <boost/unordered_set.hpp>
#include <vector>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
    class Accumulator : public RefCountable {
    public:
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
            dassert(_memUsageBytes != 0); // This would mean subclass didn't set it
            return _memUsageBytes;
        }

        /// Reset this accumulator to a fresh state ready to receive input.
        virtual void reset() = 0;

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

    private:
        typedef boost::unordered_set<Value, Value::Hash> SetType;
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

    private:
        BSONType totalType;
        long long longTotal;
        double doubleTotal;
    };


    class AccumulatorMinMax final : public Accumulator {
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

        static boost::intrusive_ptr<Accumulator> createMin();
        static boost::intrusive_ptr<Accumulator> createMax();

    private:
        Value _val;
        const Sense _sense;
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


    class AccumulatorStdDev final : public Accumulator {
    public:
        explicit AccumulatorStdDev(bool isSamp);

        void processInternal(const Value& input, bool merging) final;
        Value getValue(bool toBeMerged) const final;
        const char* getOpName() const final;
        void reset() final;

        static boost::intrusive_ptr<Accumulator> createSamp();
        static boost::intrusive_ptr<Accumulator> createPop();

    private:
        const bool _isSamp;
        long long _count;
        double _mean;
        double _m2; // Running sum of squares of delta from mean. Named to match algorithm.
    };
}
