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

#include "db/pipeline/expression.h"
#include "bson/bsontypes.h"

namespace mongo {
    class Accumulator :
        public ExpressionNary {
    public:
        // virtuals from ExpressionNary
        virtual void addOperand(shared_ptr<Expression> pExpression);
	virtual void addToBsonObj(
	    BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const;
	virtual void addToBsonArray(
	    BSONArrayBuilder *pBuilder, bool fieldPrefix) const;

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
	    BSONObjBuilder *pBuilder, string fieldName, string opName) const;
    };


    class AccumulatorAppend :
        public Accumulator {
    public:
        // virtuals from Expression
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
        virtual shared_ptr<const Value> getValue() const;
	virtual const char *getName() const;

        /*
          Create an appending accumulator.

          @returns the created accumulator
         */
        static shared_ptr<Accumulator> create();

    private:
        AccumulatorAppend();

        mutable vector<shared_ptr<const Value>> vpValue;
    };


    class AccumulatorMinMax :
        public Accumulator {
    public:
        // virtuals from Expression
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
        virtual shared_ptr<const Value> getValue() const;
	virtual const char *getName() const;

        /*
          Create a summing accumulator.

          @returns the created accumulator
         */
        static shared_ptr<Accumulator> createMin();
        static shared_ptr<Accumulator> createMax();

    private:
        AccumulatorMinMax(int theSense);

        int sense; /* 1 for min, -1 for max; used to "scale" comparison */

        mutable shared_ptr<const Value> pValue; /* current min/max */
    };


    class AccumulatorSum :
        public Accumulator {
    public:
        // virtuals from Accumulator
        virtual shared_ptr<const Value> evaluate(
            shared_ptr<Document> pDocument) const;
        virtual shared_ptr<const Value> getValue() const;
	virtual const char *getName() const;

        /*
          Create a summing accumulator.

          @returns the created accumulator
         */
        static shared_ptr<Accumulator> create();

    private:
        AccumulatorSum();

        mutable BSONType resultType;
        mutable long long longResult;
        mutable double doubleResult;
    };
}
