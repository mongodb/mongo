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
    void AccumulatorPush::processInternal(const Value& input) {
        if (!pCtx->getDoingMerge()) {
            if (!input.missing()) {
                vpValue.push_back(input);
            }
        }
        else {
            /*
              If we're in the router, we need to take apart the arrays we
              receive and put their elements into the array we are collecting.
              If we didn't, then we'd get an array of arrays, with one array
              from each shard that responds.
             */
            verify(input.getType() == Array);
            
            const vector<Value>& vec = input.getArray();
            vpValue.insert(vpValue.end(), vec.begin(), vec.end());
        }
    }

    Value AccumulatorPush::getValue() const {
        return Value(vpValue);
    }

    AccumulatorPush::AccumulatorPush(
        const intrusive_ptr<ExpressionContext> &pTheCtx):
        Accumulator(),
        vpValue(),
        pCtx(pTheCtx) {
    }

    intrusive_ptr<Accumulator> AccumulatorPush::create(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<AccumulatorPush> pAccumulator(
            new AccumulatorPush(pCtx));
        return pAccumulator;
    }

    const char *AccumulatorPush::getOpName() const {
        return "$push";
    }
}
