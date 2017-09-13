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

/*
 * This library supports a templating language that helps in generating BSON documents from a
 * template. The language supports the following templates:
 * #RAND_INT, #SEQ_INT, #RAND_STRING, #CONCAT, #CUR_DATE, $VARIABLE and #OID.
 *
 * The language will help in quickly expressing richer documents  for use in benchRun.
 * Ex. : { key : { #RAND_INT: [10, 20] } } or  { key : { #CONCAT: ["hello", " ", "world"] } }
 *
 * Where possible, the templates can also be combined together and evaluated. For eg.
 * { key : { #CONCAT: [{ #RAND_INT: [10, 20] }, " ", "world"] } }
 *
 * This library DOES NOT support combining or nesting the templates in an arbitrary fashion.
 * eg. { key : { #RAND_INT: [{ #RAND_INT: [10, 15] }, 20] } } is not supported.
 *
 */
#pragma once

#include <map>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/functional.h"

namespace mongo {

/*
 * BsonTemplateEvaluator Object for evaluating the templates. The Object exposes
 * methods to evaluate existing template operators (#RAND_INT) and add new template operators.
 *
 * To evaluate a template, call the object's 'evaluate' method and pass it as arguments, the
 * template object that you want to evaluate and a BSONObjBuilder object that will contain the
 * resultant BSON object. Eg.
 *
 * Status st = bsonTemplEvalObj->evaluate(inputTemplateObj, outputBSONObjBuilder)
 *
 * The 'evaluate' method will never throw an exception and will return an appropriate Status
 * code on success/error scenario.
 *
 * High level working : The evaluate() method takes in a BSONObj as input, iterates over the
 * BSON elements in the input BSONObj, and calls _evalElem() method. The _evalElem() method
 * figures out the specific template and then calls the corresponding template function.
 * The evaluated result is added to the BSONObjBuilder object and is returned to the evaluate()
 * method.
 *
 */
class BsonTemplateEvaluator {
    MONGO_DISALLOW_COPYING(BsonTemplateEvaluator);

public:
    /* Status of template evaluation. Logically the  the status are "success", "bad operator"
     * and "operation evaluation error." */
    enum Status { StatusSuccess = 0, StatusBadOperator, StatusOpEvaluationError };

    /*
     * OperatorFn : function object wrappers that define a call interface.
     * All template operators have this signature.
     * @params btl : pointer to the BsonTemplateEvaluator Object
     *         fieldName : key of the object being evaluated
     *         in : the embedded BSONObj
     *         builder : the output BSONObj
     *  Eg. for object { key : { #RAND_INT: [10, 20] } }
     *      fieldName : key
     *      in : { #RAND_INT: [10, 20] }
     */
    typedef stdx::function<Status(BsonTemplateEvaluator* btl,
                                  const char* fieldName,
                                  const BSONObj& in,
                                  BSONObjBuilder& builder)>
        OperatorFn;

    /*
     * @params seed : Random seed to be used when generating random data
     */
    BsonTemplateEvaluator(int64_t seed);
    ~BsonTemplateEvaluator();

    /**
     * Set an identifying number for this template evaluator.
     */
    Status setId(size_t id);

    /*
     * "Add a new operator, "name" with behavior "op" to this evaluator.
     */
    void addOperator(const std::string& name, const OperatorFn& op);

    /*
     * Returns the OperatorFn registered for the operator named "op", or
     * OperatorFn() if there is no such operator.
     */
    OperatorFn operatorEvaluator(const std::string& op) const;

    /* This is the top level method for using this library. It takes a BSON Object as input,
     * evaluates the templates and saves the result in the builder object.
     * The method returns a status code on success/error condition.
     * The templates cannot be used at the top level.
     * So this is okay as an input { key : {#RAND_INT: [10, 20]} }
     * but not this { {#RAND_INT : [10, 20]} : some_value }
     */
    Status evaluate(const BSONObj& src, BSONObjBuilder& builder);

    /**
     * Sets the given variable
     */
    void setVariable(const std::string& name, const BSONElement& elem);

private:
    void initializeEvaluator();
    // map that holds operators along with their respective function pointers
    typedef std::map<std::string, OperatorFn> OperatorMap;
    OperatorMap _operatorFunctions;

    // map that holds variable name and value pairs
    typedef std::map<std::string, BSONObj> VarMap;
    VarMap _varMap;

    // evaluates a BSON element. This is internally called by the top level evaluate method.
    Status _evalElem(const BSONElement in, BSONObjBuilder& out);

    // evaluates a BSON object. This is internally called by the top level evaluate method
    // and the _evalElem method.
    Status _evalObj(const BSONObj& in, BSONObjBuilder& out);

    // An identifier for this template evaluator instance, which distinguishes it
    // from other evaluators. Useful for threaded benchruns which, say, want to insert
    // similar sequences of values without colliding with one another.
    unsigned char _id;

    // Keeps state for each SEQ_INT expansion being evaluated by this bson template evaluator
    // instance. Maps from the seq_id of the sequence to its current value.
    std::map<int, long long> _seqIdMap;

    /*
     * Operator method to support #RAND_INT :  { key : { #RAND_INT: [10, 20] } }
     * The array arguments to #RAND_INT are the min and mix range between which a random number
     * will be chosen. The chosen random number is inclusive at the lower end but not at the
     * upper end.
     * This will evaluate to something like { key : 14 }
     * #RAND_INT also supports a third optional argument which is a multiplier.
     * Thus for an input { key : { #RAND_INT: [10, 20, 4] } }, the method will
     * choose a random number between 10 and 20 and then multiple the chosen value with 4.
     */
    static Status evalRandInt(BsonTemplateEvaluator* btl,
                              const char* fieldName,
                              const BSONObj& in,
                              BSONObjBuilder& out);

    /*
     * Operator method to support
     *  #RAND_INT_PLUS_THREAD : { key : { #RAND_INT_PLUS_THREAD: [10, 20] } }
     * See #RAND_INT above for definition. This variation differs from the base in the
     * it uses the upper bound of the requested range to segment the ranges by
     * the thread_id of the TemplateEvaluator - thus
     * thread 0 [0, 1000] yields 0...999
     * thread 1 [0, 1000] yields 1000...1999
     * etc.
     */
    static Status evalRandPlusThread(BsonTemplateEvaluator* btl,
                                     const char* fieldName,
                                     const BSONObj& in,
                                     BSONObjBuilder& out);

    /*
     * Operator method to support #SEQ_INT :
     *    { key : { #SEQ_INT: { seq_id: 0, start: 100, step: -2, unique: true } } }
     *
     * Used to generate arithmetic sequences of integers in each successive template
     * evaluation. The 'seq_id' identifies this expansion so that the same document can
     * have multiple expansions. The sequence will begin with the value 'start' and
     * then increment by 'step' for each successive expansion.
     *
     * If 'unique: true' is specified, then the sequences are adjusted according to the
     * _id of this template evaluator so that various worker threads don't overrun each
     * other's ranges.
     *
     * If using with multiple threads, each thread should have its own BsonTemplateEvaluator
     * instance so that the sequence state remains separate.
     *
     * If 'mod: <num>' is specified, then modulo <num> is applied to each value of the
     * arithmetic sequence.
     *
     * Ex. { a: { #SEQ_INT: { seq_id: 0, start: 4, step: 3 } } } will generate
     *    { a: 4 }, { a: 7 }, { a: 10 }, etc.
     */
    static Status evalSeqInt(BsonTemplateEvaluator* btl,
                             const char* fieldName,
                             const BSONObj& in,
                             BSONObjBuilder& out);
    /*
     * Operator method to support #RAND_STRING : { key : { #RAND_STRING: [12] } }
     * The array argument to RAND_STRING is the length of the std::string that is desired.
     * This will evaluate to something like { key : "randomstring" }
     */
    static Status evalRandString(BsonTemplateEvaluator* btl,
                                 const char* fieldName,
                                 const BSONObj& in,
                                 BSONObjBuilder& out);
    /*
     * Operator method to support #CONCAT : { key : { #CONCAT: ["hello", " ", "world", 2012] } }
     * The array argument to CONCAT are the strings to be concatenated. If the argument is not
     * a std::string it will be stringified and concatendated.
     * This will evaluate to { key : "hello world2012" }
     */
    static Status evalConcat(BsonTemplateEvaluator* btl,
                             const char* fieldName,
                             const BSONObj& in,
                             BSONObjBuilder& out);
    /*
     * Operator method to support #OID : { _id : { #OID: 1 } }
     * The 'key' field is required to be _id, but the argument to OID does not matter,
     * and will be neither examined nor validated.
     */
    static Status evalObjId(BsonTemplateEvaluator* btl,
                            const char* fieldName,
                            const BSONObj& in,
                            BSONObjBuilder& out);

    /*
     * Operator method to support variables: {_id: { #VARIABLE: "x" } }
     *
     */
    static Status evalVariable(BsonTemplateEvaluator* btl,
                               const char* fieldName,
                               const BSONObj& in,
                               BSONObjBuilder& out);

    /*
     * Operator method to support #CUR_DATE : { date: { #CUR_DATE: 100 }
     * The argument to CUR_DATE is the offset in milliseconds which will be added to the current
     * date.  Pass an offset of 0 to get the current date.
     */
    static Status evalCurrentDate(BsonTemplateEvaluator* btl,
                                  const char* fieldName,
                                  const BSONObj& in,
                                  BSONObjBuilder& out);

    // Per object pseudo random number generator
    PseudoRandom rng;
};

}  // end namespace
