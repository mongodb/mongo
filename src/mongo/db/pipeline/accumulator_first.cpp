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

#include "db/pipeline/value.h"

namespace mongo {

    intrusive_ptr<const Value> AccumulatorFirst::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        verify(vpOperand.size() == 1);

        /* only remember the first value seen */
        if (!pValue.get())
            pValue = vpOperand[0]->evaluate(pDocument);

        return pValue;
    }

    AccumulatorFirst::AccumulatorFirst():
        AccumulatorSingleValue() {
    }

    intrusive_ptr<Accumulator> AccumulatorFirst::create(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<AccumulatorFirst> pAccumulator(
            new AccumulatorFirst());
        return pAccumulator;
    }

    const char *AccumulatorFirst::getOpName() const {
        return "$first";
    }
}
