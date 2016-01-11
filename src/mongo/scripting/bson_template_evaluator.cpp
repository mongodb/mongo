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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/scripting/bson_template_evaluator.h"

#include <cstddef>
#include <cstdlib>

#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

void BsonTemplateEvaluator::initializeEvaluator() {
    addOperator("RAND_INT", &BsonTemplateEvaluator::evalRandInt);
    addOperator("RAND_INT_PLUS_THREAD", &BsonTemplateEvaluator::evalRandPlusThread);
    addOperator("SEQ_INT", &BsonTemplateEvaluator::evalSeqInt);
    addOperator("RAND_STRING", &BsonTemplateEvaluator::evalRandString);
    addOperator("CONCAT", &BsonTemplateEvaluator::evalConcat);
    addOperator("OID", &BsonTemplateEvaluator::evalObjId);
    addOperator("VARIABLE", &BsonTemplateEvaluator::evalVariable);
    addOperator("CUR_DATE", &BsonTemplateEvaluator::evalCurrentDate);
}

BsonTemplateEvaluator::BsonTemplateEvaluator(int64_t seed) : _id(0), rng(seed) {
    initializeEvaluator();
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::setId(size_t id) {
    if (id >= (1 << 7))
        // The id is too large--doesn't fit inside 7 bits.
        return StatusOpEvaluationError;
    this->_id = id;
    return StatusSuccess;
}

BsonTemplateEvaluator::~BsonTemplateEvaluator() {}

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

/* Sets the variable
 */
void BsonTemplateEvaluator::setVariable(const std::string& name, const BSONElement& elem) {
    this->_varMap[name] = elem.wrap();
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::_evalElem(const BSONElement in,
                                                               BSONObjBuilder& out) {
    if (in.type() == Array) {
        BSONArrayBuilder arrayBuilder(out.subarrayStart(in.fieldName()));
        std::vector<BSONElement> arrElems = in.Array();
        for (unsigned int i = 0; i < arrElems.size(); i++) {
            BSONElement element = arrElems[i];
            if (element.isABSONObj()) {
                BSONObjBuilder newBuilder(element.objsize());
                Status st = _evalElem(element, newBuilder);
                if (st != StatusSuccess)
                    return st;
                // Only want to append the field value, not the whole object
                arrayBuilder.append(newBuilder.done().getField(element.fieldName()));
            } else {
                arrayBuilder.append(element);
            }
        }
        arrayBuilder.done();
        return StatusSuccess;
    }

    if (in.type() != Object) {
        out.append(in);
        return StatusSuccess;
    }

    // continue evaluation on subObject
    BSONObj subObj = in.embeddedObject();

    BSONObjBuilder newBuilder(subObj.objsize() + 128);
    Status st = _evalObj(subObj, newBuilder);
    if (st != StatusSuccess)
        return st;

    BSONObj updatedObj = newBuilder.obj();

    // check if updated object needs template evaluation
    const char* opOrNot = updatedObj.firstElementFieldName();
    if (opOrNot[0] == '#') {
        const char* op = opOrNot + 1;
        OperatorFn fn = operatorEvaluator(op);
        if (!fn)
            return StatusBadOperator;
        Status st = fn(this, in.fieldName(), updatedObj, out);
        if (st != StatusSuccess)
            return st;
    } else {
        out.append(in.fieldName(), updatedObj);
    }
    return StatusSuccess;
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::_evalObj(const BSONObj& in,
                                                              BSONObjBuilder& out) {
    BSONForEach(e, in) {
        Status st = _evalElem(e, out);
        if (st != StatusSuccess)
            return st;
    }
    return StatusSuccess;
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalRandInt(BsonTemplateEvaluator* btl,
                                                                 const char* fieldName,
                                                                 const BSONObj& in,
                                                                 BSONObjBuilder& out) {
    // in = { #RAND_INT: [10, 20] }
    BSONObj range = in.firstElement().embeddedObject();
    if (!range["0"].isNumber() || !range["1"].isNumber())
        return StatusOpEvaluationError;
    const int min = range["0"].numberInt();
    const int max = range["1"].numberInt();
    if (max <= min)
        return StatusOpEvaluationError;
    // range of max-min
    int randomNum = min + (btl->rng.nextInt32(max - min));
    if (range.nFields() == 3) {
        if (!range[2].isNumber())
            return StatusOpEvaluationError;
        randomNum *= range[2].numberInt();
    }
    out.append(fieldName, randomNum);
    return StatusSuccess;
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalRandPlusThread(BsonTemplateEvaluator* btl,
                                                                        const char* fieldName,
                                                                        const BSONObj& in,
                                                                        BSONObjBuilder& out) {
    // in = { #RAND_INT_PLUS_THREAD: [10, 20] }
    BSONObj range = in.firstElement().embeddedObject();
    if (!range["0"].isNumber() || !range["1"].isNumber())
        return StatusOpEvaluationError;
    const int min = range["0"].numberInt();
    const int max = range["1"].numberInt();
    if (max <= min)
        return StatusOpEvaluationError;
    int randomNum = min + (btl->rng.nextInt32(max - min));
    randomNum += ((max - min) * btl->_id);
    out.append(fieldName, randomNum);
    return StatusSuccess;
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalSeqInt(BsonTemplateEvaluator* btl,
                                                                const char* fieldName,
                                                                const BSONObj& in,
                                                                BSONObjBuilder& out) {
    // in = { #SEQ_INT: { seq_id: 0, start: 10, step: -2 }
    BSONObj spec = in.firstElement().embeddedObject();
    if (spec.nFields() < 3)
        return StatusOpEvaluationError;
    if (spec["seq_id"].eoo() || !spec["seq_id"].isNumber())
        return StatusOpEvaluationError;
    if (spec["start"].eoo() || !spec["start"].isNumber())
        return StatusOpEvaluationError;
    if (spec["step"].eoo() || !spec["step"].isNumber())
        return StatusOpEvaluationError;

    // If we're here, then we have a well-formed SEQ_INT specification:
    // seq_id, start, and step fields, which are all numbers.

    int seq_id = spec["seq_id"].numberInt();
    long long curr_seqval = spec["start"].numberInt();

    // Handle the optional "unique" argument.
    //
    // If the test requires us to keep sequences unique between different
    // worker threads, then put the id number of this evaluator in the
    // high order byte.
    if (!spec["unique"].eoo() && spec["unique"].trueValue()) {
        long long workerid = btl->_id;
        curr_seqval += (workerid << ((sizeof(long long) - 1) * 8));
    }

    if (btl->_seqIdMap.end() != btl->_seqIdMap.find(seq_id)) {
        // We already have a sequence value. Add 'step' to get the next value.
        int step = spec["step"].numberInt();
        curr_seqval = btl->_seqIdMap[seq_id] + step;
    }

    // Handle the optional "mod" argument. This should be done after
    // handling all other options (currently just "unique").
    if (!spec["mod"].eoo()) {
        if (!spec["mod"].isNumber())
            return StatusOpEvaluationError;
        int modval = spec["mod"].numberInt();
        if (modval <= 0)
            return StatusOpEvaluationError;
        curr_seqval = (curr_seqval % modval);
    }

    // Store the sequence value.
    btl->_seqIdMap[seq_id] = curr_seqval;

    out.append(fieldName, curr_seqval);
    return StatusSuccess;
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalRandString(BsonTemplateEvaluator* btl,
                                                                    const char* fieldName,
                                                                    const BSONObj& in,
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
    static const char alphanum[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    static const size_t alphaNumLength = sizeof(alphanum) - 1;
    static_assert(alphaNumLength == 64, "alphaNumLength == 64");
    uint32_t currentRand = 0;
    std::string str;
    for (int i = 0; i < length; ++i, currentRand >>= 6) {
        if (i % 5 == 0)
            currentRand = btl->rng.nextInt32();
        str.push_back(alphanum[currentRand % alphaNumLength]);
    }
    out.append(fieldName, str);
    return StatusSuccess;
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalConcat(BsonTemplateEvaluator* btl,
                                                                const char* fieldName,
                                                                const BSONObj& in,
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
            part.toString(stringBuilder, false);
    }
    out.append(fieldName, stringBuilder.str());
    return StatusSuccess;
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalObjId(BsonTemplateEvaluator* btl,
                                                               const char* fieldName,
                                                               const BSONObj& in,
                                                               BSONObjBuilder& out) {
    // in = { #OID: 1 }
    if (!mongoutils::str::equals(fieldName, "_id"))
        // Error: must be generating a value for the _id field.
        return StatusOpEvaluationError;
    out.genOID();
    return StatusSuccess;
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalVariable(BsonTemplateEvaluator* btl,
                                                                  const char* fieldName,
                                                                  const BSONObj& in,
                                                                  BSONObjBuilder& out) {
    // in = { #VARIABLE: "x" }
    BSONElement varEle = btl->_varMap[in["#VARIABLE"].String()].firstElement();
    if (!varEle.eoo()) {
        out.appendAs(varEle, fieldName);
        return StatusSuccess;
    } else {
        return StatusOpEvaluationError;
    }
}

BsonTemplateEvaluator::Status BsonTemplateEvaluator::evalCurrentDate(BsonTemplateEvaluator* btl,
                                                                     const char* fieldName,
                                                                     const BSONObj& in,
                                                                     BSONObjBuilder& out) {
    // in = { #CUR_DATE: 1 }
    auto offset = Milliseconds(in.firstElement().numberLong());
    out.append(fieldName, Date_t::now() + offset);
    return StatusSuccess;
}

}  // end namespace mongo
