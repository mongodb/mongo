/*
 *    Copyright (C) 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/*
 * This library supports a templating language that helps in generating BSON documents from a
 * template. The language supports the following template:
 * #RAND_INT, #RAND_STRING and #CONCAT.
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

#include <boost/function.hpp>
#include <boost/noncopyable.hpp>

#include "mongo/db/jsobj.h"

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
    class BsonTemplateEvaluator : private boost::noncopyable {
    public:
        /* Status of template evaluation. Logically the  the status are "success", "bad operator"
         * and "operation evaluation error." */
        enum Status {
            StatusSuccess = 0,
            StatusBadOperator,
            StatusOpEvaluationError
        };

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
        typedef boost::function< Status (BsonTemplateEvaluator* btl, const char* fieldName,
                                         const BSONObj& in, BSONObjBuilder& builder) > OperatorFn;

        BsonTemplateEvaluator();
        ~BsonTemplateEvaluator();

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

    private:
        void initializeEvaluator();
        // map that holds operators along with their respective function pointers
        typedef std::map< std::string, OperatorFn > OperatorMap;
        OperatorMap _operatorFunctions;

        // evaluates a BSON element. This is internally called by the top level evaluate method.
        Status _evalElem(BSONElement in, BSONObjBuilder& out);

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
        static Status evalRandInt(BsonTemplateEvaluator* btl, const char* fieldName,
                                  const BSONObj in, BSONObjBuilder& out);
        /*
         * Operator method to support #RAND_STRING : { key : { #RAND_STRING: [12] } }
         * The array argument to RAND_STRING is the length of the string that is desired.
         * This will evaluate to something like { key : "randomstring" }
         */
        static Status evalRandString(BsonTemplateEvaluator* btl, const char* fieldName,
                                     const BSONObj in, BSONObjBuilder& out);
        /*
         * Operator method to support #CONCAT : { key : { #CONCAT: ["hello", " ", "world", 2012] } }
         * The array argument to CONCAT are the strings to be concatenated. If the argument is not
         * a string it will be stringified and concatendated.
         * This will evaluate to { key : "hello world2012" }
         */
        static Status evalConcat(BsonTemplateEvaluator* btl, const char* fieldName,
                                 const BSONObj in, BSONObjBuilder& out);

    };

} // end namespace
