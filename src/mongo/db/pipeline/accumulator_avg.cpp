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

#include "db/pipeline/document.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"

namespace mongo {

    const char AccumulatorAvg::subTotalName[] = "subTotal";
    const char AccumulatorAvg::countName[] = "count";

    Value AccumulatorAvg::evaluate(const Document& pDocument) const {
        if (!pCtx->getDoingMerge()) {
            Super::evaluate(pDocument);
        }
        else {
            /*
              If we're in the router, we expect an object that contains
              both a subtotal and a count.  This is what getValue() produced
              below.
             */
            Value shardOut = vpOperand[0]->evaluate(pDocument);
            verify(shardOut.getType() == Object);

            Value subTotal = shardOut[subTotalName];
            verify(!subTotal.missing());
            doubleTotal += subTotal.getDouble();
                
            Value subCount = shardOut[countName];
            verify(!subCount.missing());
            count += subCount.getLong();
        }

        return Value();
    }

    intrusive_ptr<Accumulator> AccumulatorAvg::create(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<AccumulatorAvg> pA(new AccumulatorAvg(pCtx));
        return pA;
    }

    Value AccumulatorAvg::getValue() const {
        if (!pCtx->getInShard()) {
            double avg = 0;
            if (count)
                avg = doubleTotal / static_cast<double>(count);

            return Value(avg);
        }

        MutableDocument out;
        out.addField(subTotalName, Value(doubleTotal));
        out.addField(countName, Value(count));

        return Value(out.freeze());
    }

    AccumulatorAvg::AccumulatorAvg(
        const intrusive_ptr<ExpressionContext> &pTheCtx):
        AccumulatorSum(),
        pCtx(pTheCtx) {
    }

    const char *AccumulatorAvg::getOpName() const {
        return "$avg";
    }
}
