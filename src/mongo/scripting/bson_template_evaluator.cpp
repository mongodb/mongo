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

#include <cstddef>
#include <cstdlib>

#include "mongo/util/map_util.h"

namespace mongo {

    void BsonTemplateEvaluator::initializeEvaluator() {
        addOperator("RAND_INT", &BsonTemplateEvaluator::evalRandInt);
        addOperator("RAND_STRING", &BsonTemplateEvaluator::evalRandString);
        addOperator("CONCAT", &BsonTemplateEvaluator::evalConcat);
    }

    BsonTemplateEvaluator::BsonTemplateEvaluator() {
        initializeEvaluator();
    }

    BsonTemplateEvaluator::~BsonTemplateEvaluator() { }

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
        int randomNum = min + (rand() % (max - min));
        if (range.nFields() == 3) {
            if (!range[2].isNumber())
                return StatusOpEvaluationError;
            randomNum *= range[2].numberInt();
        }
        out.append(fieldName, randomNum);
        return StatusSuccess;
    }

    BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalRandString(BsonTemplateEvaluator* btl,
                                                                        const char* fieldName,
                                                                        const BSONObj in,
                                                                        BSONObjBuilder& out) {
        // in = { #RAND_STRING: [10] }
        BSONObj range = in.firstElement().embeddedObject();
        if (range.nFields() != 1)
            return StatusOpEvaluationError;
        if (!range[0].isNumber())
            return StatusOpEvaluationError;
        const int length = range["0"].numberInt();
        if (length <= 0)
            return StatusOpEvaluationError;
        static const char alphanum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                       "abcdefghijklmnopqrstuvwxyz"
                                       "0123456789+/";
        static const size_t alphaNumLength = sizeof(alphanum) - 1;
        BOOST_STATIC_ASSERT(alphaNumLength == 64);
        unsigned currentRand = 0;
        std::string str;
        for (int i = 0; i < length; ++i, currentRand >>= 6) {
            if (i % 5 == 0)
                currentRand = rand();
            str.push_back(alphanum[currentRand % alphaNumLength]);
        }
        out.append(fieldName, str);
        return StatusSuccess;
    }

    BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalConcat(BsonTemplateEvaluator* btl,
                                                                    const char* fieldName,
                                                                    const BSONObj in,
                                                                    BSONObjBuilder& out) {
        // in = { #CONCAT: ["hello", " ", "world"] }
        BSONObjBuilder objectBuilder;
        Status status = btl->evaluate(in.firstElement().embeddedObject(), objectBuilder);
        if (status != StatusSuccess)
            return status;
        BSONObj parts = objectBuilder.obj();
        if (parts.nFields() <= 1)
            return StatusOpEvaluationError;
        StringBuilder stringBuilder;
        BSONForEach(part, parts) {
            if (part.type() == String)
                stringBuilder << part.String();
            else
                part.toString(stringBuilder,false);
        }
        out.append(fieldName, stringBuilder.str());
        return StatusSuccess;
    }

} // end namespace mongo
