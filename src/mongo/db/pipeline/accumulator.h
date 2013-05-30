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
 */

#pragma once

#include "mongo/pch.h"

#include <boost/unordered_set.hpp>
#include "db/pipeline/value.h"
#include "db/pipeline/expression.h"
#include "bson/bsontypes.h"

namespace mongo {
    class ExpressionContext;

    class Accumulator : public RefCountable {
    public:
        /// Serialize Accumulator and its input generating Expression.
        Value serialize() const;

        // TODO rename to process()
        /// Process input and update internal state.
        void evaluate(const Document& input) { processInternal(_expr->evaluate(input)); }

        /// Add needed fields to the passed in set
        void addDependencies(set<string>& deps) const { _expr->addDependencies(deps); }

        // TODO move into constructor
        /// Sets the expression used to generate input to the accumulator.
        void addOperand(const intrusive_ptr<Expression>& expr) {
            verify(!_expr); // only takes one operand
            _expr = expr;
        }

        /// Marks the end of the evaluate() phase and return accumulated result.
        virtual Value getValue() const = 0;

        /// The name of the op as used in a serialization of the pipeline.
        virtual const char* getOpName() const = 0;

    protected:
        Accumulator() {}

        /// Update subclass's internal state based on input
        virtual void processInternal(const Value& input) = 0;

    private:
        intrusive_ptr<Expression> _expr;
    };


    class AccumulatorAddToSet : public Accumulator {
    public:
        virtual void processInternal(const Value& input);
        virtual Value getValue() const;
        virtual const char* getOpName() const;

        /*
          Create an appending accumulator.

          @param pCtx the expression context
          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorAddToSet(const intrusive_ptr<ExpressionContext> &pTheCtx);
        typedef boost::unordered_set<Value, Value::Hash> SetType;
        SetType set;
        intrusive_ptr<ExpressionContext> pCtx;
    };


    class AccumulatorFirst : public Accumulator {
    public:
        virtual void processInternal(const Value& input);
        virtual Value getValue() const;
        virtual const char* getOpName() const;

        /*
          Create the accumulator.

          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorFirst();

        bool _haveFirst;
        Value _first;
    };


    class AccumulatorLast : public Accumulator {
    public:
        virtual void processInternal(const Value& input);
        virtual Value getValue() const;
        virtual const char* getOpName() const;

        /*
          Create the accumulator.

          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorLast();
        Value _last;
    };


    class AccumulatorSum : public Accumulator {
    public:
        virtual void processInternal(const Value& input);
        virtual Value getValue() const;
        virtual const char* getOpName() const;

        /*
          Create a summing accumulator.

          @param pCtx the expression context
          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

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
        virtual void processInternal(const Value& input);
        virtual Value getValue() const;
        virtual const char* getOpName() const;

        /*
          Create either the max or min accumulator.

          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> createMin(
            const intrusive_ptr<ExpressionContext> &pCtx);
        static intrusive_ptr<Accumulator> createMax(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorMinMax(int theSense);

        Value _val;
        const int _sense; /* 1 for min, -1 for max; used to "scale" comparison */
    };


    class AccumulatorPush : public Accumulator {
    public:
        virtual void processInternal(const Value& input);
        virtual Value getValue() const;
        virtual const char* getOpName() const;

        /*
          Create an appending accumulator.

          @param pCtx the expression context
          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorPush(const intrusive_ptr<ExpressionContext> &pTheCtx);

        vector<Value> vpValue;
        intrusive_ptr<ExpressionContext> pCtx;
    };


    class AccumulatorAvg : public AccumulatorSum {
        typedef AccumulatorSum Super;
    public:
        virtual void processInternal(const Value& input);
        virtual Value getValue() const;
        virtual const char* getOpName() const;

        /*
          Create an averaging accumulator.

          @param pCtx the expression context
          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        static const char subTotalName[];
        static const char countName[];

        AccumulatorAvg(const intrusive_ptr<ExpressionContext> &pCtx);

        intrusive_ptr<ExpressionContext> pCtx;
    };

}
