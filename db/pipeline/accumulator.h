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
	virtual void addOperand(const shared_ptr<Expression> &pExpression);
	virtual void addToBsonObj(
	    BSONObjBuilder *pBuilder, string fieldName, unsigned depth) const;
	virtual void addToBsonArray(
	    BSONArrayBuilder *pBuilder, unsigned depth) const;

        /*
          Get the accumulated value.

          @returns the accumulated value
         */
        virtual shared_ptr<const Value> getValue() const = 0;

    protected:
        Accumulator();

	/*
	  Convenience method for doing this for accumulators.  The pattern
	  is always the same, so a common implementation works, but requires
	  knowing the operator name.

	  @params pBuilder the builder to add to
	  @params fieldName the projected name
	  @params opName the operator name
	 */
	void opToBson(
	    BSONObjBuilder *pBuilder, string fieldName, string opName,
	    unsigned depth) const;
    };


    class AccumulatorAddToSet :
        public Accumulator {
    public:
        // virtuals from Expression
	virtual shared_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual shared_ptr<const Value> getValue() const;
	virtual const char *getOpName() const;

        /*
          Create an appending accumulator.

	  @param pCtx the expression context
          @returns the created accumulator
         */
        static shared_ptr<Accumulator> create(
	    const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorAddToSet(const intrusive_ptr<ExpressionContext> &pTheCtx);
        typedef boost::unordered_set<shared_ptr<const Value>, Value::Hash > SetType;
        mutable SetType set;
        mutable SetType::iterator itr; 
	intrusive_ptr<ExpressionContext> pCtx;
    };


    class AccumulatorMinMax :
        public Accumulator {
    public:
        // virtuals from Expression
	virtual shared_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual shared_ptr<const Value> getValue() const;
	virtual const char *getOpName() const;

        /*
          Create a summing accumulator.

          @returns the created accumulator
         */
        static shared_ptr<Accumulator> createMin(
	    const intrusive_ptr<ExpressionContext> &pCtx);
        static shared_ptr<Accumulator> createMax(
	    const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorMinMax(int theSense);

        int sense; /* 1 for min, -1 for max; used to "scale" comparison */

        mutable shared_ptr<const Value> pValue; /* current min/max */
    };


    class AccumulatorPush :
        public Accumulator {
    public:
        // virtuals from Expression
	virtual shared_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual shared_ptr<const Value> getValue() const;
	virtual const char *getOpName() const;

        /*
          Create an appending accumulator.

	  @param pCtx the expression context
          @returns the created accumulator
         */
        static shared_ptr<Accumulator> create(
	    const intrusive_ptr<ExpressionContext> &pCtx);

    private:
        AccumulatorPush(const intrusive_ptr<ExpressionContext> &pTheCtx);

        mutable vector<shared_ptr<const Value> > vpValue;
	intrusive_ptr<ExpressionContext> pCtx;
    };


    class AccumulatorSum :
        public Accumulator {
    public:
        // virtuals from Accumulator
	virtual shared_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual shared_ptr<const Value> getValue() const;
	virtual const char *getOpName() const;

        /*
          Create a summing accumulator.

	  @param pCtx the expression context
          @returns the created accumulator
         */
        static shared_ptr<Accumulator> create(
	    const intrusive_ptr<ExpressionContext> &pCtx);

    protected: /* reused by AccumulatorAvg */
        AccumulatorSum();

        mutable BSONType totalType;
        mutable long long longTotal;
        mutable double doubleTotal;
    };


    class AccumulatorAvg :
	public AccumulatorSum {
        typedef AccumulatorSum Super;
    public:
        // virtuals from Accumulator
	virtual shared_ptr<const Value> evaluate(
            const intrusive_ptr<Document> &pDocument) const;
        virtual shared_ptr<const Value> getValue() const;
	virtual const char *getOpName() const;

        /*
          Create an averaging accumulator.

	  @param pCtx the expression context
          @returns the created accumulator
         */
        static shared_ptr<Accumulator> create(
	    const intrusive_ptr<ExpressionContext> &pCtx);

    private:
	static const char subTotalName[];
	static const char countName[];

        AccumulatorAvg(const intrusive_ptr<ExpressionContext> &pCtx);

	mutable long long count;
	intrusive_ptr<ExpressionContext> pCtx;
    };

}
