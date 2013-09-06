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

#include "mongo/pch.h"

#include <boost/unordered_set.hpp>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
    class Accumulator : public RefCountable {
    public:
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
        Accumulator() : _memUsageBytes(0) {}

        /// Update subclass's internal state based on input
        virtual void processInternal(const Value& input, bool merging) = 0;

        /// subclasses are expected to update this as necessary
        int _memUsageBytes;
    };


    class AccumulatorAddToSet : public Accumulator {
    public:
        virtual void processInternal(const Value& input, bool merging);
        virtual Value getValue(bool toBeMerged) const;
        virtual const char* getOpName() const;
        virtual void reset();

        static intrusive_ptr<Accumulator> create();

    private:
        AccumulatorAddToSet();
        typedef boost::unordered_set<Value, Value::Hash> SetType;
        SetType set;
    };


    class AccumulatorFirst : public Accumulator {
    public:
        virtual void processInternal(const Value& input, bool merging);
        virtual Value getValue(bool toBeMerged) const;
        virtual const char* getOpName() const;
        virtual void reset();

        static intrusive_ptr<Accumulator> create();

    private:
        AccumulatorFirst();

        bool _haveFirst;
        Value _first;
    };


    class AccumulatorLast : public Accumulator {
    public:
        virtual void processInternal(const Value& input, bool merging);
        virtual Value getValue(bool toBeMerged) const;
        virtual const char* getOpName() const;
        virtual void reset();

        static intrusive_ptr<Accumulator> create();

    private:
        AccumulatorLast();
        Value _last;
    };


    class AccumulatorSum : public Accumulator {
    public:
        virtual void processInternal(const Value& input, bool merging);
        virtual Value getValue(bool toBeMerged) const;
        virtual const char* getOpName() const;
        virtual void reset();

        static intrusive_ptr<Accumulator> create();

    protected: /* reused by AccumulatorAvg */
        AccumulatorSum();

        BSONType totalType;
        long long longTotal;
        double doubleTotal;
        // count is only used by AccumulatorAvg, but lives here to avoid counting non-numeric values
        long long count;
    };


    class AccumulatorMinMax : public Accumulator {
    public:
        virtual void processInternal(const Value& input, bool merging);
        virtual Value getValue(bool toBeMerged) const;
        virtual const char* getOpName() const;
        virtual void reset();

        static intrusive_ptr<Accumulator> createMin();
        static intrusive_ptr<Accumulator> createMax();

    private:
        AccumulatorMinMax(int theSense);

        Value _val;
        const int _sense; /* 1 for min, -1 for max; used to "scale" comparison */
    };


    class AccumulatorPush : public Accumulator {
    public:
        virtual void processInternal(const Value& input, bool merging);
        virtual Value getValue(bool toBeMerged) const;
        virtual const char* getOpName() const;
        virtual void reset();

        static intrusive_ptr<Accumulator> create();

    private:
        AccumulatorPush();

        vector<Value> vpValue;
    };


    class AccumulatorAvg : public AccumulatorSum {
        typedef AccumulatorSum Super;
    public:
        virtual void processInternal(const Value& input, bool merging);
        virtual Value getValue(bool toBeMerged) const;
        virtual const char* getOpName() const;
        virtual void reset();

        static intrusive_ptr<Accumulator> create();

    private:
        AccumulatorAvg();
    };
}
