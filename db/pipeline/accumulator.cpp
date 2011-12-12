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
#include "db/pipeline/accumulator.h"

#include "db/jsobj.h"

namespace mongo {

    void Accumulator::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        assert(vpOperand.size() < 1); // CW TODO error: no more than one arg
        ExpressionNary::addOperand(pExpression);
    }

    Accumulator::Accumulator():
        ExpressionNary() {
    }

    void Accumulator::opToBson(
	BSONObjBuilder *pBuilder, string opName,
	string fieldName, unsigned depth) const {
	assert(vpOperand.size() == 1);
	BSONObjBuilder builder;
	vpOperand[0]->addToBsonObj(&builder, opName, depth);
	pBuilder->append(fieldName, builder.done());
    }

    void Accumulator::addToBsonObj(
	BSONObjBuilder *pBuilder, string fieldName, unsigned depth) const {
	opToBson(pBuilder, getOpName(), fieldName, depth);
    }

    void Accumulator::addToBsonArray(
	BSONArrayBuilder *pBuilder, unsigned depth) const {
	assert(false); // these can't appear in arrays
    }

    void agg_framework_reservedErrors()
    {
	uassert(15943, "reserved error", false);
	uassert(15944, "reserved error", false);
	uassert(15945, "reserved error", false);
	uassert(15946, "reserved error", false);
	uassert(15947, "reserved error", false);
	uassert(15948, "reserved error", false);
	uassert(15949, "reserved error", false);
	uassert(15950, "reserved error", false);
	uassert(15951, "reserved error", false);
	uassert(15952, "reserved error", false);
	uassert(15953, "reserved error", false);
	uassert(15954, "reserved error", false);
	uassert(15955, "reserved error", false);
	uassert(15956, "reserved error", false);
	uassert(15957, "reserved error", false);
	uassert(15958, "reserved error", false);
	uassert(15959, "reserved error", false);
	uassert(15960, "reserved error", false);
	uassert(15961, "reserved error", false);
    }
}
