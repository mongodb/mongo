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

#include "pch.h"
#include "accumulator.h"

#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"

namespace mongo {
    Value AccumulatorAddToSet::evaluate(const Document& pDocument) const {
        verify(vpOperand.size() == 1);
        Value prhs(vpOperand[0]->evaluate(pDocument));

        if (!pCtx->getDoingMerge()) {
            if (!prhs.missing()) {
                set.insert(prhs);
            }
        } else {
            /*
              If we're in the router, we need to take apart the arrays we
              receive and put their elements into the array we are collecting.
              If we didn't, then we'd get an array of arrays, with one array
              from each shard that responds.
             */
            verify(prhs.getType() == Array);
            
            const vector<Value>& array = prhs.getArray();
            set.insert(array.begin(), array.end());
        }

        return Value();
    }

    Value AccumulatorAddToSet::getValue() const {
        vector<Value> valVec;

        for (itr = set.begin(); itr != set.end(); ++itr) {
            valVec.push_back(*itr);
        }

        return Value(valVec);
    }

    AccumulatorAddToSet::AccumulatorAddToSet(
        const intrusive_ptr<ExpressionContext> &pTheCtx):
        Accumulator(),
        set(),
        pCtx(pTheCtx) {
    }

    intrusive_ptr<Accumulator> AccumulatorAddToSet::create(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<AccumulatorAddToSet> pAccumulator(
            new AccumulatorAddToSet(pCtx));
        return pAccumulator;
    }

    const char *AccumulatorAddToSet::getOpName() const {
        return "$addToSet";
    }
}
