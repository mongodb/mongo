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
        boost::shared_ptr<Expression> pExpression) {
        assert(vpOperand.size() < 1); // CW TODO error: no more than one arg
        ExpressionNary::addOperand(pExpression);
    }

    Accumulator::Accumulator():
        ExpressionNary() {
    }

    void Accumulator::opToBson(
	BSONObjBuilder *pBuilder, string opName, string fieldName) const {
	assert(vpOperand.size() == 1);
	BSONObjBuilder builder;
	vpOperand[0]->addToBsonObj(&builder, opName, true);
	pBuilder->append(fieldName, builder.done());
    }

    void Accumulator::addToBsonObj(
	BSONObjBuilder *pBuilder, string fieldName, bool docPrefix) const {
	opToBson(pBuilder, getName(), fieldName);
    }

    void Accumulator::addToBsonArray(
	BSONArrayBuilder *pBuilder, bool fieldPrefix) const {
	assert(false); // these can't appear in arrays
    }

}
