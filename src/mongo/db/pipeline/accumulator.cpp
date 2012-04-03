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
        string fieldName) const {
        verify(vpOperand.size() == 1);
        BSONObjBuilder builder;
        vpOperand[0]->addToBsonObj(&builder, opName, false);
        pBuilder->append(fieldName, builder.done());
    }

    void Accumulator::addToBsonObj(
        BSONObjBuilder *pBuilder, string fieldName,
        bool requireExpression) const {
        opToBson(pBuilder, getOpName(), fieldName);
    }

    void Accumulator::addToBsonArray(BSONArrayBuilder *pBuilder) const {
        verify(false); // these can't appear in arrays
    }

    void agg_framework_reservedErrors() {
        uassert(16030, "reserved error", false);
        uassert(16031, "reserved error", false);
        uassert(16032, "reserved error", false);
        uassert(16033, "reserved error", false);

        uassert(16036, "reserved error", false);
        uassert(16037, "reserved error", false);
        uassert(16038, "reserved error", false);
        uassert(16039, "reserved error", false);
        uassert(16040, "reserved error", false);
        uassert(16041, "reserved error", false);
        uassert(16042, "reserved error", false);
        uassert(16043, "reserved error", false);
        uassert(16044, "reserved error", false);
        uassert(16045, "reserved error", false);
        uassert(16046, "reserved error", false);
        uassert(16047, "reserved error", false);
        uassert(16048, "reserved error", false);
        uassert(16049, "reserved error", false);
    }
}
