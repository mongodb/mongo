/** @file mongo/scripting/templateevaluator.cpp */

/*
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/scripting/bson_template_evaluator.h"

#include <cstdlib>

#include "mongo/util/map_util.h"

namespace mongo {

    void BsonTemplateEvaluator::initializeEvaluator() {
        addOperator("RAND_INT", &BsonTemplateEvaluator::evalRandInt);
    }

    BsonTemplateEvaluator::BsonTemplateEvaluator() {
        initializeEvaluator();
    }

    void BsonTemplateEvaluator::addOperator(const std::string& name, const OperatorFn& op) {
        _operatorFunctions[name] = op;
    }

    BsonTemplateEvaluator::OperatorFn BsonTemplateEvaluator::operatorEvaluator(
        const std::string& op) const {
        return mapFindWithDefault(_operatorFunctions, op, OperatorFn());
    }

    /* This is the top level method for using this library. It takes a BSON Object as input,
     * evaluates the templates and saves the result in the builder object.
     * The method returns appropriate Status on success/error condition.
     */
    BsonTemplateEvaluator::Status BsonTemplateEvaluator::evaluate(const BSONObj& in,
                                                                  BSONObjBuilder& builder) {
        BSONForEach(e, in) {
            Status st = _evalElem(e, builder);
            if (st != StatusSuccess)
                return st;
        }
        return StatusSuccess;
    }

    BsonTemplateEvaluator::Status BsonTemplateEvaluator::_evalElem(BSONElement in,
                                                                   BSONObjBuilder& out) {
       if (in.type() != Object) {
           out.append(in);
           return StatusSuccess;
       }
       BSONObj subObj = in.embeddedObject();
       const char* opOrNot = subObj.firstElementFieldName();
       if (opOrNot[0] != '#') {
           out.append(in);
           return StatusSuccess;
       }
       const char* op = opOrNot+1;
       OperatorFn fn = operatorEvaluator(op);
       if (!fn)
           return StatusBadOperator;
       Status st = fn(this, in.fieldName(), subObj, out);
       if (st != StatusSuccess)
           return st;
       return StatusSuccess;
   }

    BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalRandInt(BsonTemplateEvaluator* btl,
                                                                     const char* fieldName,
                                                                     const BSONObj in,
                                                                     BSONObjBuilder& out) {
        // in = { #RAND_INT: [10, 20] }
        BSONObj range = in.firstElement().embeddedObject();
        if (!range[0].isNumber() || !range[1].isNumber())
            return StatusOpEvaluationError;
        const int min = range["0"].numberInt();
        const int max  = range["1"].numberInt();
        if (max <= min)
            return StatusOpEvaluationError;
        int randomNum = min + (random() % (max - min));
        if (range.nFields() == 3) {
            if (!range[2].isNumber())
                return StatusOpEvaluationError;
            randomNum *= range[2].numberInt();
        }
        out.append(fieldName, randomNum);
        return StatusSuccess;
    }

} // end namespace mongo
