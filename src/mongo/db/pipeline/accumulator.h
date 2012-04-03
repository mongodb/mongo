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

#include "pch.h"

#include <boost/unordered_set.hpp>
#include "db/pipeline/value.h"
#include "db/pipeline/expression.h"
#include "bson/bsontypes.h"

namespace mongo {
    class ExpressionContext;

    class Accumulator :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual void addOperand(const intrusive_ptr<Expression> &pExpression);
        virtual void addToBsonObj(
            BSONObjBuilder *pBuilder, string fieldName,
            bool requireExpression) const;
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder) const;

        /*
          Get the accumulated value.

          @returns the accumulated value
         */
        virtual intrusive_ptr<const Value> getValue() const = 0;

    protected:
        Accumulator();

        /*
          Convenience method for doing this for accumulators.  The pattern
          is always the same, so a common implementation works, but requires
          knowing the operator name.

          @param pBuilder the builder to add to
          @param fieldName the projected name
          @param opName the operator name
         */
        void opToBson(
            BSONObjBuilder *pBuilder, string fieldName, string opName) const;
    };


    class AccumulatorAddToSet :
        public Accumulator {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual intrusive_ptr<const Value> getValue() const;
        virtual const char *getOpName() const;

        /*
          Create an appending accumulator.

          @param pCtx the expression context
          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorAddToSet(const intrusive_ptr<ExpressionContext> &pTheCtx);
        typedef boost::unordered_set<intrusive_ptr<const Value>, Value::Hash > SetType;
        mutable SetType set;
        mutable SetType::iterator itr; 
        intrusive_ptr<ExpressionContext> pCtx;
    };


    /*
      This isn't a finished accumulator, but rather a convenient base class
      for others such as $first, $last, $max, $min, and similar.  It just
      provides a holder for a single Value, and the getter for that.  The
      holder is protected so derived classes can manipulate it.
     */
    class AccumulatorSingleValue :
        public Accumulator {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<const Value> getValue() const;

    protected:
        AccumulatorSingleValue();

        mutable intrusive_ptr<const Value> pValue; /* current min/max */
    };


    class AccumulatorFirst :
        public AccumulatorSingleValue {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;

        /*
          Create the accumulator.

          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorFirst();
    };


    class AccumulatorLast :
        public AccumulatorSingleValue {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;

        /*
          Create the accumulator.

          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorLast();
    };


    class AccumulatorSum :
        public Accumulator {
    public:
        // virtuals from Accumulator
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual intrusive_ptr<const Value> getValue() const;
        virtual const char *getOpName() const;

        /*
          Create a summing accumulator.

          @param pCtx the expression context
          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    protected: /* reused by AccumulatorAvg */
        AccumulatorSum();

        mutable BSONType totalType;
        mutable long long longTotal;
        mutable double doubleTotal;
    };


    class AccumulatorMinMax :
        public AccumulatorSingleValue {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual const char *getOpName() const;

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

        int sense; /* 1 for min, -1 for max; used to "scale" comparison */
    };


    class AccumulatorPush :
        public Accumulator {
    public:
        // virtuals from Expression
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual intrusive_ptr<const Value> getValue() const;
        virtual const char *getOpName() const;

        /*
          Create an appending accumulator.

          @param pCtx the expression context
          @returns the created accumulator
         */
        static intrusive_ptr<Accumulator> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorPush(const intrusive_ptr<ExpressionContext> &pTheCtx);

        mutable vector<intrusive_ptr<const Value> > vpValue;
        intrusive_ptr<ExpressionContext> pCtx;
    };


    class AccumulatorAvg :
        public AccumulatorSum {
        typedef AccumulatorSum Super;
    public:
        // virtuals from Accumulator
        virtual intrusive_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual intrusive_ptr<const Value> getValue() const;
        virtual const char *getOpName() const;

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

        mutable long long count;
        intrusive_ptr<ExpressionContext> pCtx;
    };

}
