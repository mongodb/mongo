/**
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
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::string;

namespace {

void common_rand_tests(string op, BsonTemplateEvaluator* t) {
    // Test failure when the arguments are not integers
    BSONObjBuilder builder1;
    BSONObj randObj = BSON(op << BSON_ARRAY("hello"
                                            << "world"));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t->evaluate(BSON("randField" << randObj), builder1));

    // Test failure when operator does not exists
    BSONObjBuilder builder2;
    randObj = BSON("#RAND_OP_NOT_EXISTS" << BSON_ARRAY(5 << 0));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusBadOperator,
                  t->evaluate(BSON("randField" << randObj), builder2));

    // Test failure when arguments are not correct (max < min)
    BSONObjBuilder builder3;
    randObj = BSON(op << BSON_ARRAY(5 << 0));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t->evaluate(BSON("randField" << randObj), builder3));

    // Test failure when operators are arbitrarily nested
    // {id: { #op: [ { #op: [0, 5] }, 10] }
    BSONObjBuilder builder4;
    BSONObj innerRandObj = BSON(op << BSON_ARRAY(0 << 5));
    BSONObj outerRandObj = BSON(op << BSON_ARRAY(innerRandObj << 10));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t->evaluate(BSON("randField" << outerRandObj), builder4));
}

TEST(BSONTemplateEvaluatorTest, RAND_INT) {
    BsonTemplateEvaluator t(1234567);
    int randValue1, randValue2;

    common_rand_tests("#RAND_INT", &t);

    // Test success with a single element
    BSONObjBuilder builder5;
    BSONObj randObj = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField" << randObj), builder5));
    BSONObj obj5 = builder5.obj();
    ASSERT_EQUALS(obj5.nFields(), 1);
    randValue1 = obj5["randField"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);

    // Test success with two #RAND_INT elements
    BSONObjBuilder builder6;
    randObj = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField1" << randObj << "randField2" << randObj), builder6));

    // Test success with #RAND_INT as first element
    BSONObjBuilder builder8;
    randObj = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField" << randObj << "hello"
                                              << "world"
                                              << "id"
                                              << 1),
                             builder8));
    BSONObj obj8 = builder8.obj();
    ASSERT_EQUALS(obj8.nFields(), 3);
    randValue1 = obj8["randField"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);

    // Test success with #RAND_INT as the middle element
    BSONObjBuilder builder9;
    randObj = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("id" << 1 << "randField" << randObj << "hello"
                                       << "world"),
                             builder9));
    BSONObj obj9 = builder9.obj();
    ASSERT_EQUALS(obj9.nFields(), 3);
    randValue1 = obj9["randField"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);

    // Test success with #RAND_INT as the first and the last element
    BSONObjBuilder builder10;
    randObj = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField1" << randObj << "hello"
                                               << "world"
                                               << "randField2"
                                               << randObj),
                             builder10));
    BSONObj obj10 = builder10.obj();
    ASSERT_EQUALS(obj10.nFields(), 3);
    randValue1 = obj10["randField1"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);
    randValue2 = obj10["randField2"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue2, 1);
    ASSERT_LESS_THAN(randValue2, 5);

    // Test success when one of the element is an array
    BSONObjBuilder builder11;
    randObj = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("testArray" << BSON_ARRAY(0 << 5 << 10 << 20) << "hello"
                                              << "world"
                                              << "randField"
                                              << randObj),
                             builder11));
    BSONObj obj11 = builder11.obj();
    ASSERT_EQUALS(obj11.nFields(), 3);
    randValue1 = obj11["randField"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);

    // Test success with a 3rd argument to #RAND_INT
    BSONObjBuilder builder12;
    randObj = BSON("#RAND_INT" << BSON_ARRAY(1 << 5 << 4));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("id" << randObj), builder12));
    BSONObj obj12 = builder12.obj();
    ASSERT_EQUALS(obj12.nFields(), 1);
    randValue1 = obj12["id"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 4);
    ASSERT_LESS_THAN_OR_EQUALS(randValue1, 16);
}

TEST(BSONTemplateEvaluatorTest, RAND_INT_PLUS_THREAD) {
    BsonTemplateEvaluator t(2345678);
    t.setId(1);
    int randValue1, randValue2;

    common_rand_tests("#RAND_INT_PLUS_THREAD", &t);

    // Test success with a single element
    BSONObjBuilder builder5;
    BSONObj randObj = BSON("#RAND_INT_PLUS_THREAD" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField" << randObj), builder5));
    BSONObj obj5 = builder5.obj();
    ASSERT_EQUALS(obj5.nFields(), 1);
    randValue1 = obj5["randField"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 5);
    ASSERT_LESS_THAN(randValue1, 10);

    // Test success with two #RAND_INT_PLUS_THREAD elements
    BSONObjBuilder builder6;
    randObj = BSON("#RAND_INT_PLUS_THREAD" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField1" << randObj << "randField2" << randObj), builder6));

    // Test success with #RAND_INT_PLUS_THREAD as first element
    BSONObjBuilder builder8;
    randObj = BSON("#RAND_INT_PLUS_THREAD" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField" << randObj << "hello"
                                              << "world"
                                              << "id"
                                              << 1),
                             builder8));
    BSONObj obj8 = builder8.obj();
    ASSERT_EQUALS(obj8.nFields(), 3);
    randValue1 = obj8["randField"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 5);
    ASSERT_LESS_THAN(randValue1, 10);

    // Test success with #RAND_INT_PLUS_THREAD as the middle element
    BSONObjBuilder builder9;
    randObj = BSON("#RAND_INT_PLUS_THREAD" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("id" << 1 << "randField" << randObj << "hello"
                                       << "world"),
                             builder9));
    BSONObj obj9 = builder9.obj();
    ASSERT_EQUALS(obj9.nFields(), 3);
    randValue1 = obj9["randField"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 5);
    ASSERT_LESS_THAN(randValue1, 10);

    // Test success with #RAND_INT_PLUS_THREAD as the first and the last element
    BSONObjBuilder builder10;
    randObj = BSON("#RAND_INT_PLUS_THREAD" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField1" << randObj << "hello"
                                               << "world"
                                               << "randField2"
                                               << randObj),
                             builder10));
    BSONObj obj10 = builder10.obj();
    ASSERT_EQUALS(obj10.nFields(), 3);

    BSONObjIterator iter10(obj10);
    randValue1 = obj10["randField1"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 5);
    ASSERT_LESS_THAN(randValue1, 10);
    randValue2 = obj10["randField2"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue2, 5);
    ASSERT_LESS_THAN(randValue2, 10);

    // Test success when one of the element is an array
    BSONObjBuilder builder11;
    randObj = BSON("#RAND_INT_PLUS_THREAD" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("testArray" << BSON_ARRAY(0 << 5 << 10 << 20) << "hello"
                                              << "world"
                                              << "randField"
                                              << randObj),
                             builder11));
    BSONObj obj11 = builder11.obj();
    ASSERT_EQUALS(obj11.nFields(), 3);
    randValue1 = obj11["randField"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 5);
    ASSERT_LESS_THAN(randValue1, 10);

    // Test success with a 3rd argument to #RAND_INT_PLUS_THREAD to confirm its ignored
    BSONObjBuilder builder12;
    randObj = BSON("#RAND_INT_PLUS_THREAD" << BSON_ARRAY(1 << 5 << 4));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("id" << randObj), builder12));
    BSONObj obj12 = builder12.obj();
    ASSERT_EQUALS(obj12.nFields(), 1);
    randValue1 = obj12["id"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 5);
    ASSERT_LESS_THAN_OR_EQUALS(randValue1, 10);

    // Test success with a single element for a zero _id
    BsonTemplateEvaluator t2(3456789);
    t2.setId(0);

    BSONObjBuilder builder13;
    randObj = BSON("#RAND_INT_PLUS_THREAD" << BSON_ARRAY(1 << 5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t2.evaluate(BSON("randField" << randObj), builder13));
    BSONObj obj13 = builder13.obj();
    ASSERT_EQUALS(obj13.nFields(), 1);
    randValue1 = obj13["randField"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);
}

TEST(BSONTemplateEvaluatorTest, SEQ_INT) {
    std::unique_ptr<BsonTemplateEvaluator> t(new BsonTemplateEvaluator(131415));
    BSONObj seqObj;
    BSONObj expectedObj;

    // Error if missing 'step'.
    BSONObjBuilder builder1;
    seqObj = BSON("#SEQ_INT" << BSON("seq_id" << 0 << "start" << 0));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t->evaluate(BSON("seqField" << seqObj), builder1));

    // Error if missing 'start'.
    BSONObjBuilder builder2;
    seqObj = BSON("#SEQ_INT" << BSON("seq_id" << 0 << "step" << 1));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t->evaluate(BSON("seqField" << seqObj), builder2));

    // Error if missing 'seq_iq'.
    BSONObjBuilder builder3;
    seqObj = BSON("#SEQ_INT" << BSON("start" << 0 << "step" << 1));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t->evaluate(BSON("seqField" << seqObj), builder3));

    // Error if 'step' is not a number.
    BSONObjBuilder builder4;
    seqObj = BSON("#SEQ_INT" << BSON("seq_id" << 0 << "start" << 0 << "step"
                                              << "foo"));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t->evaluate(BSON("seqField" << seqObj), builder4));

    // Error if 'start' is not a number.
    BSONObjBuilder builder5;
    seqObj = BSON("#SEQ_INT" << BSON("seq_id" << 0 << "start" << true << "step" << 1));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t->evaluate(BSON("seqField" << seqObj), builder5));

    // Error if 'seq_id' is not a number.
    BSONObjBuilder builder6;
    seqObj = BSON("#SEQ_INT" << BSON("seq_id" << BSON("foo" << 1) << "start" << 0 << "step" << 1));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t->evaluate(BSON("seqField" << seqObj), builder6));

    // Error if 'mod' is not a number.
    BSONObjBuilder builder7;
    seqObj = BSON("#SEQ_INT" << BSON("seq_id" << 0 << "start" << 0 << "step" << 1 << "mod"
                                              << "foo"));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t->evaluate(BSON("seqField" << seqObj), builder7));

    // Test an increasing sequence: -4, -2, 0, 2, ...
    seqObj = BSON("#SEQ_INT" << BSON("seq_id" << 0 << "start" << -4 << "step" << 2));
    for (int i = -4; i <= 2; i += 2) {
        BSONObjBuilder builder8;
        ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                      t->evaluate(BSON("seqField" << seqObj), builder8));
        expectedObj = BSON("seqField" << i);
        ASSERT_EQUALS(0, expectedObj.woCompare(builder8.obj()));
    }

    // Test a decreasing sequence: 5, 0, -5, -10
    seqObj = BSON("#SEQ_INT" << BSON("seq_id" << 1 << "start" << 5 << "step" << -5));
    for (int i = 5; i >= -10; i -= 5) {
        BSONObjBuilder builder9;
        ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                      t->evaluate(BSON("seqField" << seqObj), builder9));
        expectedObj = BSON("seqField" << i);
        ASSERT_EQUALS(0, expectedObj.woCompare(builder9.obj()));
    }

    // Test multiple sequences in the same document. In order for this to
    // work the two sequences must have different sequence IDs.
    //
    // seq_id 2: 0, 1, 2, 3, ...
    // seq_id 3: 0, -1, -2, -3, ...
    BSONObj seqObj1 = BSON("#SEQ_INT" << BSON("seq_id" << 2 << "start" << 0 << "step" << 1));
    BSONObj seqObj2 = BSON("#SEQ_INT" << BSON("seq_id" << 3 << "start" << 0 << "step" << -1));
    BSONObj seqObjFull = BSON("seqField1" << seqObj1 << "seqField2" << seqObj2);
    for (int i = 0; i <= 3; i++) {
        BSONObjBuilder builder10;
        ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess, t->evaluate(seqObjFull, builder10));
        expectedObj = BSON("seqField1" << i << "seqField2" << -i);
        ASSERT_EQUALS(0, expectedObj.woCompare(builder10.obj()));
    }

    // Test that the 'unique: true' option correctly puts the ID of the
    // bson template evaluator into the high order byte of a 64 bit integer.
    long long evaluatorId = 9;
    t->setId(evaluatorId);
    seqObj1 = BSON("#SEQ_INT" << BSON("seq_id" << 4 << "start" << 8 << "step" << 1));
    seqObj2 =
        BSON("#SEQ_INT" << BSON("seq_id" << 5 << "start" << 8 << "step" << 1 << "unique" << true));

    // Without 'unique: true'.
    BSONObjBuilder builder11;
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t->evaluate(BSON("seqField" << seqObj1), builder11));
    expectedObj = BSON("seqField" << 8);
    ASSERT_EQUALS(0, expectedObj.woCompare(builder11.obj()));

    // With 'unique: true'.
    BSONObjBuilder builder12;
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t->evaluate(BSON("seqField" << seqObj2), builder12));
    // The template evaluator id of 9 goes in the high-order byte.
    long long expectedSeqNum = (evaluatorId << ((sizeof(long long) - 1) * 8)) + 8;
    expectedObj = BSON("seqField" << expectedSeqNum);
    ASSERT_EQUALS(0, expectedObj.woCompare(builder12.obj()));

    // Test a sequence using "mod": 0, 1, 2, 0, 1
    seqObj = BSON("#SEQ_INT" << BSON("seq_id" << 6 << "start" << 0 << "step" << 1 << "mod" << 3));
    for (int i = 0; i <= 5; i++) {
        BSONObjBuilder builder13;
        ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                      t->evaluate(BSON("seqField" << seqObj), builder13));
        expectedObj = BSON("seqField" << (i % 3));
        ASSERT_EQUALS(0, expectedObj.woCompare(builder13.obj()));
    }

    // Test that you can't set an id if it is more than 7 bits wide.
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess, t->setId(127));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError, t->setId(128));
}

TEST(BSONTemplateEvaluatorTest, RAND_STRING) {
    BsonTemplateEvaluator t(4567890);

    // Test failure when the arguments to RAND_STRING is not an integer
    BSONObjBuilder builder1;
    BSONObj randObj = BSON("#RAND_STRING" << BSON_ARRAY("hello"));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t.evaluate(BSON("randField" << randObj), builder1));

    // Test failure when there is more than 1 argument to RAND_STRING
    BSONObjBuilder builder2;
    randObj = BSON("#RAND_STRING" << BSON_ARRAY(2 << 8));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t.evaluate(BSON("randField" << randObj), builder2));

    // Test failure when length argument to RAND_STRING is 0
    BSONObjBuilder builder3;
    randObj = BSON("#RAND_STRING" << BSON_ARRAY(0));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t.evaluate(BSON("randField" << randObj), builder3));

    // Test success with a single element
    BSONObjBuilder builder4;
    randObj = BSON("#RAND_STRING" << BSON_ARRAY(5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField" << randObj), builder4));
    BSONObj obj4 = builder4.obj();
    ASSERT_EQUALS(obj4.nFields(), 1);
    ASSERT_EQUALS(obj4.firstElement().str().length(), 5U);

    // Test success with two #RAND_STRING elements
    BSONObjBuilder builder5;
    randObj = BSON("#RAND_STRING" << BSON_ARRAY(5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField1" << randObj << "randField2" << randObj), builder5));
    BSONObj obj5 = builder5.obj();
    ASSERT_EQUALS(obj5.nFields(), 2);
    BSONObjIterator iter5(obj5);
    ASSERT_EQUALS(iter5.next().str().length(), 5U);
    ASSERT_EQUALS(iter5.next().str().length(), 5U);

    // Test success with #RAND_STRING as the last element
    BSONObjBuilder builder6;
    randObj = BSON("#RAND_STRING" << BSON_ARRAY(5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("id" << 1 << "hello"
                                       << "world"
                                       << "randField"
                                       << randObj),
                             builder6));
    BSONObj obj6 = builder6.obj();
    ASSERT_EQUALS(obj6.nFields(), 3);
    BSONObjIterator iter6(obj6);
    iter6++;
    ASSERT_EQUALS(iter6.next().str().length(), 5U);

    // Test success with #RAND_STRING as first element
    BSONObjBuilder builder7;
    randObj = BSON("#RAND_STRING" << BSON_ARRAY(5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField" << randObj << "hello"
                                              << "world"
                                              << "id"
                                              << 1),
                             builder7));
    BSONObj obj7 = builder7.obj();
    ASSERT_EQUALS(obj7.nFields(), 3);
    ASSERT_EQUALS(obj7.firstElement().str().length(), 5U);

    // Test success with #RAND_STRING as the middle element
    BSONObjBuilder builder8;
    randObj = BSON("#RAND_STRING" << BSON_ARRAY(5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("id" << 1 << "randField" << randObj << "hello"
                                       << "world"),
                             builder8));
    BSONObj obj8 = builder8.obj();
    ASSERT_EQUALS(obj8.nFields(), 3);
    BSONObjIterator iter8(obj8);
    iter8++;
    ASSERT_EQUALS((*iter8).str().length(), 5U);

    // Test success with #RAND_STRING as the first and the last element
    BSONObjBuilder builder10;
    randObj = BSON("#RAND_STRING" << BSON_ARRAY(5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField1" << randObj << "hello"
                                               << "world"
                                               << "randField2"
                                               << randObj),
                             builder10));
    BSONObj obj10 = builder10.obj();
    ASSERT_EQUALS(obj10.nFields(), 3);

    BSONObjIterator iter10(obj10);
    ASSERT_EQUALS((*iter10).str().length(), 5U);
    iter10++;
    ASSERT_EQUALS(iter10.next().str().length(), 5U);

    // Test success when one of the element is an array
    BSONObjBuilder builder11;
    randObj = BSON("#RAND_STRING" << BSON_ARRAY(5));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("testArray" << BSON_ARRAY(0 << 5 << 10 << 20) << "hello"
                                              << "world"
                                              << "randField"
                                              << randObj),
                             builder11));
    BSONObj obj11 = builder11.obj();
    ASSERT_EQUALS(obj11.nFields(), 3);
    BSONObjIterator iter11(obj11);
    iter11++;
    ASSERT_EQUALS(iter11.next().str().length(), 5U);
}

TEST(BSONTemplateEvaluatorTest, CONCAT) {
    BsonTemplateEvaluator t(5678901);

    // Test failure when the arguments to #CONCAT has only one argument
    BSONObjBuilder builder1;
    BSONObj concatObj = BSON("#CONCAT" << BSON_ARRAY("hello"));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t.evaluate(BSON("concatField" << concatObj), builder1));

    // Test success when all arguments to #CONCAT are strings
    BSONObjBuilder builder2;
    concatObj = BSON("#CONCAT" << BSON_ARRAY("hello"
                                             << " "
                                             << "world"));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("concatField" << concatObj), builder2));
    BSONObj obj2 = builder2.obj();
    ASSERT_EQUALS(obj2.nFields(), 1);
    BSONObj expectedObj = BSON("concatField"
                               << "hello world");
    ASSERT_EQUALS(obj2.equal(expectedObj), true);

    // Test success when some arguments to #CONCAT are integers
    BSONObjBuilder builder3;
    concatObj = BSON("#CONCAT" << BSON_ARRAY("F" << 1 << "racing"));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("concatField" << concatObj), builder3));
    BSONObj obj3 = builder3.obj();
    ASSERT_EQUALS(obj3.nFields(), 1);
    expectedObj = BSON("concatField"
                       << "F1racing");
    ASSERT_EQUALS(obj3.equal(expectedObj), true);

    // Test success with #CONCAT as first element and last element
    BSONObjBuilder builder4;
    concatObj = BSON("#CONCAT" << BSON_ARRAY("hello"
                                             << " "
                                             << "world"));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("concatField1" << concatObj << "middleKey" << 1 << "concatField2"
                                                 << concatObj),
                             builder4));
    BSONObj obj4 = builder4.obj();
    ASSERT_EQUALS(obj4.nFields(), 3);
    expectedObj = BSON("concatField1"
                       << "hello world"
                       << "middleKey"
                       << 1
                       << "concatField2"
                       << "hello world");
    ASSERT_EQUALS(obj4.equal(expectedObj), true);

    // Test success when one of the arguments to #CONCAT is an array
    BSONObjBuilder builder5;
    concatObj = BSON("#CONCAT" << BSON_ARRAY("hello" << BSON_ARRAY(1 << 10) << "world"));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("concatField" << concatObj), builder5));
    BSONObj obj5 = builder5.obj();
    ASSERT_EQUALS(obj5.nFields(), 1);
    expectedObj = BSON("concatField"
                       << "hello[ 1, 10 ]world");
    ASSERT_EQUALS(obj5.equal(expectedObj), true);
}

TEST(BSONTemplateEvaluatorTest, OID) {
    BsonTemplateEvaluator t(6789012);
    BSONObj oidObj = BSON("#OID" << 1);

    // Error: field must be "_id"
    BSONObjBuilder builder1;
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t.evaluate(BSON("notIdField" << oidObj), builder1));

    // Success.
    BSONObjBuilder builder2;
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("_id" << oidObj), builder2));
}

TEST(BSONTemplateEvaluatorTest, COMBINED_OPERATORS) {
    BsonTemplateEvaluator t(7890123);
    BSONObj randIntObj = BSON("#RAND_INT" << BSON_ARRAY(0 << 5));
    BSONObj randStrObj = BSON("#RAND_STRING" << BSON_ARRAY(5));

    // Test success when  #RAND_INT, and #RAND_STRING are combined
    BSONObjBuilder builder1;
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randInt" << randIntObj << "randStr" << randStrObj), builder1));
    BSONObj obj1 = builder1.obj();
    ASSERT_EQUALS(obj1.nFields(), 2);
    BSONObjIterator iter1(obj1);
    int randInt = iter1.next().numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randInt, 0);
    ASSERT_LESS_THAN(randInt, 5);
    string randStr = iter1.next().str();
    ASSERT_EQUALS(randStr.length(), 5U);

    // Test success when the #CONCAT and #RAND_INT and #RAND_STRING are combined
    BSONObjBuilder builder2;
    BSONObj concatObj = BSON("#CONCAT" << BSON_ARRAY(randIntObj << " hello world " << randStrObj));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("concatField" << concatObj), builder2));
    BSONObj obj2 = builder2.obj();
    ASSERT_EQUALS(obj2.nFields(), 1);
    // check that the resulting string has a length of 19.
    // randIntObj.length = 1,  " hello world " = 13,  randStrObj.length = 5
    // so total string length should 1 + 13 + 5 = 19
    ASSERT_EQUALS(obj2.firstElement().str().length(), 19U);
}

// Test #VARIABLE
TEST(BSONTemplateEvaluatorTest, VARIABLE) {
    BsonTemplateEvaluator t(8901234);
    int value1;

    // Test failure when the variable has not been set
    // {id: { #VARIABLE: "foo" } }
    BSONObjBuilder builder1;
    BSONObj innerObj = BSON("#VARIABLE"
                            << "foo");
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusOpEvaluationError,
                  t.evaluate(BSON("id" << innerObj), builder1));

    // Test success when the variable has been set
    // test2 := 42
    // {id: { #VARIABLE: "test2" } }
    t.setVariable("test2", BSON("test2" << 42).getField("test2"));
    BSONObjBuilder builder2;
    innerObj = BSON("#VARIABLE"
                    << "test2");
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("id" << innerObj), builder2));
    BSONObj obj2 = builder2.obj();
    value1 = obj2["id"].numberInt();
    ASSERT_EQUALS(value1, 42);
}

// Test template recursion and other general features
TEST(BSONTemplateEvaluatorTest, NESTING) {
    BsonTemplateEvaluator t(8901234);
    int randValue1, randValue2;

    // Test failure when operators are arbitrarily nested
    // {id: { #op: [ { #op: [0, 5] }, 10] }
    BSONObjBuilder builder1;
    BSONObj innerObj = BSON("#RAND_INT" << BSON_ARRAY(0 << 5));
    BSONObj outerObj = BSON("#RAND_INT" << BSON_ARRAY(innerObj << 10));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("randField" << outerObj), builder1));

    // Test success when operators are arbitrarily nested
    // {foo: { bar: { #op: [1, 5] } } }
    BSONObjBuilder builder2;
    innerObj = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    outerObj = BSON("bar" << innerObj);
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("foo" << outerObj), builder2));
    BSONObj obj2 = builder2.obj();
    BSONElement obj2_foo = obj2["foo"];
    randValue1 = obj2_foo["bar"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);

    // Test success when operators are arbitrarily nested within multiple elements
    // {id: { foo: "hi", bar: { baz: { #op, [1, 5] } } } }
    BSONObjBuilder builder3;
    innerObj = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    BSONObj bazObj = BSON("baz" << innerObj);
    outerObj = BSON("foo"
                    << "hi"
                    << "bar"
                    << bazObj);
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("id" << outerObj), builder3));
    BSONObj obj3 = builder3.obj();
    BSONElement obj3_id = obj3["id"];
    BSONElement obj3_bar = obj3_id["bar"];
    randValue1 = obj3_bar["baz"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);

    // Test success when operators are arbitrarily nested within multiple elements
    // {id: { foo: "hi", bar: { #op: [1, 5] }, baz: { baz_a: { #op, [5, 10] }, baz_b: { #op, [10,
    // 15] }, baz_c: "bye" } }
    BSONObjBuilder builder4;
    BSONObj barObj4 = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    BSONObj bazObj4a = BSON("#RAND_INT" << BSON_ARRAY(5 << 10));
    BSONObj bazObj4b = BSON("#RAND_INT" << BSON_ARRAY(10 << 15));
    BSONObj bazObj4 = BSON("baz_a" << bazObj4a << "baz_b" << bazObj4b << "baz_c"
                                   << "bye");
    outerObj = BSON("foo"
                    << "hi"
                    << "bar"
                    << barObj4
                    << "baz"
                    << bazObj4);
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess,
                  t.evaluate(BSON("id" << outerObj), builder4));
    BSONObj obj4 = builder4.obj();
    BSONElement obj4_id = obj4["id"];
    randValue1 = obj4_id["bar"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);
    BSONElement obj4_baz = obj4_id["baz"];
    randValue1 = obj4_baz["baz_a"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 5);
    ASSERT_LESS_THAN(randValue1, 10);
    randValue1 = obj4_baz["baz_b"].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 10);
    ASSERT_LESS_THAN(randValue1, 15);

    // Test Failure for an invalid op deeper in the template
    // { op: "let", target: "x", value: {"#NOT_A_VALID_OP": [0, 1000]}}
    BSONObjBuilder builder5;
    innerObj = BSON("#NOT_A_VALID_OP" << BSON_ARRAY(0 << 1000));
    outerObj = BSON("op"
                    << "let"
                    << "target"
                    << "x"
                    << "value"
                    << innerObj);
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusBadOperator, t.evaluate(outerObj, builder5));

    // Test success for elements in an array that need evaluation
    // { foo: "hi", bar: [  { #op: [1, 5] }, { #op: [5, 10], { baz: 42 }, 7 ] }
    BSONObjBuilder builder6;
    BSONObj elem1 = BSON("#RAND_INT" << BSON_ARRAY(1 << 5));
    BSONObj elem2 = BSON("#RAND_INT" << BSON_ARRAY(5 << 10));
    BSONObj elem3 = BSON("baz" << 42);
    outerObj = BSON("foo"
                    << "hi"
                    << "bar"
                    << BSON_ARRAY(elem1 << elem2 << elem3 << 7));
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess, t.evaluate(outerObj, builder6));
    BSONObj obj6 = builder6.obj();
    BSONElement obj6_bar = obj6["bar"];
    randValue1 = obj6_bar.Obj()[0].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue1, 1);
    ASSERT_LESS_THAN(randValue1, 5);
    randValue2 = obj6_bar.Obj()[1].numberInt();
    ASSERT_GREATER_THAN_OR_EQUALS(randValue2, 5);
    ASSERT_LESS_THAN(randValue2, 10);

    // Test success for elements in an array that need evaluation
    // { foo: { #op: ["a", "b"]} , bar: "hi" }
    BSONObjBuilder builder7;
    innerObj = BSON("#CONCAT" << BSON_ARRAY("a"
                                            << "b"));
    outerObj = BSON("foo" << innerObj << "bar"
                          << "hi");
    ASSERT_EQUALS(BsonTemplateEvaluator::StatusSuccess, t.evaluate(outerObj, builder7));
    BSONObj obj7 = builder7.obj();
    BSONElement obj7_foo = obj7["foo"];
    ASSERT_EQUALS(obj7_foo.String(), "ab");
}
}  // end anonymous namespace
}  // end namespace mongo
