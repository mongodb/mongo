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
#include "util/mongoutils/str.h"

namespace mongo {
    using namespace mongoutils;

    void Accumulator::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
	uassert(15943, str::stream() << "group accumulator " <<
		getOpName() << " only accepts one operand",
		vpOperand.size() < 1);
	
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
	uassert(15984, "reserved error", false);
	uassert(15990, "reserved error", false);
	uassert(15991, "reserved error", false);
	uassert(15992, "reserved error", false);
	uassert(15993, "reserved error", false);
	uassert(15994, "reserved error", false);
	uassert(15995, "reserved error", false);
	uassert(15996, "reserved error", false);
	uassert(15997, "reserved error", false);
	uassert(15998, "reserved error", false);
	uassert(15999, "reserved error", false);
	uassert(16000, "reserved error", false);
	uassert(16001, "reserved error", false);
	uassert(16002, "reserved error", false);
	uassert(16003, "reserved error", false);
	uassert(16004, "reserved error", false);
	uassert(16005, "reserved error", false);
	uassert(16006, "reserved error", false);
	uassert(16007, "reserved error", false);
	uassert(16008, "reserved error", false);
	uassert(16009, "reserved error", false);
	uassert(16010, "reserved error", false);
	uassert(16011, "reserved error", false);
	uassert(16012, "reserved error", false);
	uassert(16013, "reserved error", false);
	uassert(16014, "reserved error", false);
	uassert(16015, "reserved error", false);
	uassert(16016, "reserved error", false);
	uassert(16017, "reserved error", false);
	uassert(16018, "reserved error", false);
	uassert(16019, "reserved error", false);
    }
}
