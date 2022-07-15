/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file contains tests for mongo/db/query/query_planner.cpp
 */


#include "mongo/db/query/query_planner_test_lib.h"

#include <ostream>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {

using namespace mongo;

using std::string;

Status filterMatches(const BSONObj& testFilter,
                     const MatchExpression* trueFilter,
                     std::unique_ptr<CollatorInterface> collator) {
    std::unique_ptr<MatchExpression> trueFilterClone(trueFilter->shallowClone());
    MatchExpression::sortTree(trueFilterClone.get());

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(testFilter, expCtx);
    if (!statusWithMatcher.isOK()) {
        return statusWithMatcher.getStatus().withContext(
            "match expression provided by the test did not parse successfully");
    }
    std::unique_ptr<MatchExpression> root = std::move(statusWithMatcher.getValue());
    if (root->matchType() == mongo::MatchExpression::NOT) {
        // Ideally we would optimize() everything, but some of the tests depend on structural
        // equivalence of single-arg $or expressions.
        root = MatchExpression::optimize(std::move(root));
    }
    MatchExpression::sortTree(root.get());
    if (trueFilterClone->equivalent(root.get())) {
        return Status::OK();
    }
    return {
        ErrorCodes::Error{5619211},
        str::stream() << "Provided filter did not match filter on query solution node. Expected: "
                      << root->toString() << ". Found: " << trueFilter->toString()};
}

Status nodeHasMatchingFilter(const BSONObj& testFilter,
                             const BSONObj& testCollation,
                             const QuerySolutionNode* trueFilterNode) {
    if (nullptr == trueFilterNode->filter) {
        return {ErrorCodes::Error{5619210}, "No filter found in query solution node"};
    }

    std::unique_ptr<CollatorInterface> testCollator;
    if (!testCollation.isEmpty()) {
        CollatorFactoryMock collatorFactoryMock;
        auto collator = collatorFactoryMock.makeFromBSON(testCollation);
        if (!collator.isOK()) {
            return collator.getStatus().withContext(
                "collation provided by the test did not parse successfully");
        }
        testCollator = std::move(collator.getValue());
    }

    return filterMatches(testFilter, trueFilterNode->filter.get(), std::move(testCollator));
}

Status columnIxScanFiltersByPathMatch(
    BSONObj expectedFiltersByPath,
    const StringMap<std::unique_ptr<MatchExpression>>& actualFiltersByPath) {

    for (auto&& expectedElem : expectedFiltersByPath) {
        const auto expectedPath = expectedElem.fieldNameStringData();
        if (expectedElem.type() != BSONType::Object)
            return {ErrorCodes::Error{6412405},
                    str::stream() << "invalid filter for path '" << expectedPath
                                  << "' given to 'filtersByPath' argument to 'column_scan' "
                                     "stage. Please specify an object. Found: "
                                  << expectedElem};

        const auto expectedFilter = expectedElem.Obj();
        if (actualFiltersByPath.contains(expectedPath)) {
            auto filterMatchStatus =
                filterMatches(expectedFilter, actualFiltersByPath.at(expectedPath).get(), nullptr);
            if (!filterMatchStatus.isOK()) {
                return filterMatchStatus.withContext(
                    str::stream() << "mismatching filter for path '" << expectedPath
                                  << "' in 'column_scan's 'filtersByPath'");
            }
        } else {
            return {ErrorCodes::Error{6412406},
                    str::stream() << "did not find an expected filter for path '" << expectedPath
                                  << "' in 'column_scan' stage. Actual filters: "
                                  << expression::filterMapToString(actualFiltersByPath)};
        }
    }
    for (auto&& [actualPath, actualFilter] : actualFiltersByPath) {
        // We already checked equality above, so just check that they were all specified.
        if (!expectedFiltersByPath.hasField(actualPath)) {
            return {ErrorCodes::Error{6412407},
                    str::stream() << "Found an unexpected filter for path '" << actualPath
                                  << "' in 'column_scan' stage. Actual filters: "
                                  << expression::filterMapToString(actualFiltersByPath)
                                  << ", expected filters: " << expectedFiltersByPath
                                  << "stage. Please specify an object."};
        }
    }
    return Status::OK();
}

template <typename Iterable>
Status stringSetsMatch(BSONElement expectedStringArrElem,
                       Iterable actualStrings,
                       std::string contextMsg) {

    stdx::unordered_set<std::string> expectedFields;
    for (auto& field : expectedStringArrElem.Array()) {
        expectedFields.insert(field.String());
    }
    if (expectedFields.size() != expectedStringArrElem.Array().size()) {
        return Status{ErrorCodes::Error{6430500}, "expected string set had duplicate elements"}
            .withContext(contextMsg);
    }

    stdx::unordered_set<std::string> actualStringsSet(actualStrings.begin(), actualStrings.end());
    if (actualStringsSet.size() != actualStrings.size()) {
        return Status{ErrorCodes::Error{6430501}, "actual string set had duplicate elements"}
            .withContext(contextMsg);
    }
    if (expectedFields != actualStringsSet) {
        return {ErrorCodes::Error{5842491}, contextMsg};
    }
    return Status::OK();
}

void appendIntervalBound(BSONObjBuilder& bob, BSONElement& el) {
    if (el.type() == String) {
        std::string data = el.String();
        if (data == "MaxKey") {
            bob.appendMaxKey("");
        } else if (data == "MinKey") {
            bob.appendMinKey("");
        } else {
            bob.appendAs(el, "");
        }
    } else {
        bob.appendAs(el, "");
    }
}

Status intervalMatches(const BSONObj& testInt, const Interval trueInt) {
    BSONObjIterator it(testInt);
    if (!it.more()) {
        return {ErrorCodes::Error{5619212},
                "Interval has no elements, expected 4 (start, end, inclusiveStart, inclusiveEnd)"};
    }
    BSONElement low = it.next();
    if (!it.more()) {
        return {
            ErrorCodes::Error{5619213},
            "Interval has only 1 element, expected 4 (start, end, inclusiveStart, inclusiveEnd)"};
    }
    BSONElement high = it.next();
    if (!it.more()) {
        return {
            ErrorCodes::Error{5619214},
            "Interval has only 2 elements, expected 4 (start, end, inclusiveStart, inclusiveEnd)"};
    }
    bool startInclusive = it.next().Bool();
    if (!it.more()) {
        return {
            ErrorCodes::Error{5619215},
            "Interval has only 3 elements, expected 4 (start, end, inclusiveStart, inclusiveEnd)"};
    }
    bool endInclusive = it.next().Bool();
    if (it.more()) {
        return {ErrorCodes::Error{5619216},
                "Interval has >4 elements, expected exactly 4: (start, end, inclusiveStart, "
                "inclusiveEnd)"};
    }

    BSONObjBuilder bob;
    appendIntervalBound(bob, low);
    appendIntervalBound(bob, high);
    Interval toCompare(bob.obj(), startInclusive, endInclusive);
    if (trueInt.equals(toCompare)) {
        return Status::OK();
    }
    return {ErrorCodes::Error{5619217},
            str::stream() << "provided interval did not match. Expected: "
                          << toCompare.toString(false) << " Found: " << trueInt.toString(false)};
}

bool bsonObjFieldsAreInSet(BSONObj obj, const std::set<std::string>& allowedFields) {
    BSONObjIterator i(obj);
    while (i.more()) {
        BSONElement child = i.next();
        if (!allowedFields.count(child.fieldName())) {
            LOGV2_ERROR(23932, "Unexpected field", "field"_attr = child.fieldName());
            return false;
        }
    }

    return true;
}

}  // namespace

namespace mongo {

/**
 * Looks in the children stored in the 'nodes' field of 'testSoln'
 * to see if thet match the 'children' field of 'trueSoln'.
 *
 * This does an unordered comparison, i.e. childrenMatch returns
 * true as long as the set of subtrees in testSoln's 'nodes' matches
 * the set of subtrees in trueSoln's 'children' vector.
 */
static Status childrenMatch(const BSONObj& testSoln,
                            const QuerySolutionNode* trueSoln,
                            bool relaxBoundsCheck) {

    BSONElement children = testSoln["nodes"];
    if (children.eoo() || !children.isABSONObj()) {
        return {ErrorCodes::Error{5619218},
                "found a stage in the solution which was expected to have 'nodes' children, but no "
                "'nodes' object in the provided JSON"};
    }

    // The order of the children array in testSoln might not match
    // the order in trueSoln, so we have to check all combos with
    // these nested loops.
    stdx::unordered_set<size_t> matchedNodeIndexes;
    BSONObjIterator i(children.Obj());
    while (i.more()) {
        BSONElement child = i.next();
        if (child.eoo() || !child.isABSONObj()) {
            return {
                ErrorCodes::Error{5619219},
                str::stream() << "found a child which was expected to be an object but was not: "
                              << child};
        }

        LOGV2_DEBUG(
            5619201, 2, "Attempting to find matching child for {plan}", "plan"_attr = child.Obj());
        // try to match against one of the QuerySolutionNode's children
        bool found = false;
        for (size_t j = 0; j < trueSoln->children.size(); ++j) {
            if (matchedNodeIndexes.find(j) != matchedNodeIndexes.end()) {
                // Do not match a child of the QuerySolutionNode more than once.
                continue;
            }
            auto matchStatus = QueryPlannerTestLib::solutionMatches(
                child.Obj(), trueSoln->children[j].get(), relaxBoundsCheck);
            if (matchStatus.isOK()) {
                LOGV2_DEBUG(5619202, 2, "Found a matching child");
                found = true;
                matchedNodeIndexes.insert(j);
                break;
            }
            LOGV2_DEBUG(5619203,
                        2,
                        "Child at index {j} did not match test solution: {reason}",
                        "j"_attr = j,
                        "reason"_attr = matchStatus.reason());
        }

        // we couldn't match child
        if (!found) {
            return {ErrorCodes::Error{5619220},
                    str::stream() << "could not find a matching plan for child: " << child};
        }
    }

    // Ensure we've matched all children of the QuerySolutionNode.
    if (matchedNodeIndexes.size() == trueSoln->children.size()) {
        return Status::OK();
    }
    return {ErrorCodes::Error{5619221},
            str::stream() << "Did not match the correct number of children. Found "
                          << matchedNodeIndexes.size() << " matching children but "
                          << trueSoln->children.size() << " children in the observed plan"};
}

Status QueryPlannerTestLib::boundsMatch(const BSONObj& testBounds,
                                        const IndexBounds trueBounds,
                                        bool relaxBoundsCheck) {
    if (testBounds.firstElementFieldName() == "$startKey"_sd) {
        if (!trueBounds.isSimpleRange) {
            return {ErrorCodes::Error{5920202}, str::stream() << "Expected bounds to be simple"};
        }

        if (testBounds.nFields() != 2) {
            return {ErrorCodes::Error{5920203},
                    str::stream() << "Expected object of form {'$startKey': ..., '$endKey': ...}"};
        }

        BSONObjIterator it(testBounds);

        {
            auto minElt = it.next();
            if (minElt.type() != BSONType::Object) {
                return {ErrorCodes::Error{5920205}, str::stream() << "Expected min obj"};
            }

            auto minObj = minElt.embeddedObject();
            if (minObj.woCompare(trueBounds.startKey)) {
                return {ErrorCodes::Error{5920201},
                        str::stream() << "'startKey' in bounds did not match. Expected "
                                      << trueBounds.startKey << " got " << minObj};
            }
        }

        {
            auto maxElt = it.next();
            if (maxElt.type() != BSONType::Object) {
                return {ErrorCodes::Error{5920204}, str::stream() << "Expected max obj"};
            }

            auto maxObj = maxElt.embeddedObject();
            if (maxObj.woCompare(trueBounds.endKey)) {
                return {ErrorCodes::Error{5920206},
                        str::stream() << "'endKey' in bounds did not match. Expected "
                                      << trueBounds.endKey << " got " << maxObj};
            }
        }

        return Status::OK();
    }

    // Iterate over the fields on which we have index bounds.
    BSONObjIterator fieldIt(testBounds);
    size_t fieldItCount = 0;
    while (fieldIt.more()) {
        BSONElement arrEl = fieldIt.next();
        if (arrEl.fieldNameStringData() != trueBounds.getFieldName(fieldItCount)) {
            return {ErrorCodes::Error{5619222},
                    str::stream() << "mismatching field name at index " << fieldItCount
                                  << ": expected '" << arrEl.fieldNameStringData()
                                  << "' but found '" << trueBounds.getFieldName(fieldItCount)
                                  << "'"};
        }
        if (arrEl.type() != Array) {
            return {ErrorCodes::Error{5619223},
                    str::stream() << "bounds are expected to be arrays. Found: " << arrEl
                                  << " (type " << arrEl.type() << ")"};
        }
        // Iterate over an ordered interval list for a particular field.
        BSONObjIterator oilIt(arrEl.Obj());
        size_t oilItCount = 0;
        while (oilIt.more()) {
            BSONElement intervalEl = oilIt.next();
            if (intervalEl.type() != Array) {
                return {ErrorCodes::Error{5619224},
                        str::stream()
                            << "intervals within bounds are expected to be arrays. Found: "
                            << intervalEl << " (type " << intervalEl.type() << ")"};
            }
            Interval trueInt = trueBounds.getInterval(fieldItCount, oilItCount);
            if (auto matchStatus = intervalMatches(intervalEl.Obj(), trueInt);
                !matchStatus.isOK()) {
                return matchStatus.withContext(
                    str::stream() << "mismatching interval found at index " << oilItCount
                                  << " within the bounds at index " << fieldItCount);
            }
            ++oilItCount;
        }

        if (!relaxBoundsCheck && oilItCount != trueBounds.getNumIntervals(fieldItCount)) {
            return {
                ErrorCodes::Error{5619225},
                str::stream() << "true bounds have more intervals than provided (bounds at index "
                              << fieldItCount << "). Expected: " << oilItCount
                              << " Found: " << trueBounds.getNumIntervals(fieldItCount)};
        }

        ++fieldItCount;
    }

    return Status::OK();
}

// static
Status QueryPlannerTestLib::solutionMatches(const BSONObj& testSoln,
                                            const QuerySolutionNode* trueSoln,
                                            bool relaxBoundsCheck) {
    //
    // leaf nodes
    //
    if (STAGE_COLLSCAN == trueSoln->getType()) {
        const CollectionScanNode* csn = static_cast<const CollectionScanNode*>(trueSoln);
        BSONElement el = testSoln["cscan"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619226},
                    "found a collection scan in the solution but no corresponding 'cscan' object "
                    "in the provided JSON"};
        }
        BSONObj csObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(csObj, {"dir", "filter", "collation"}));

        BSONElement dir = csObj["dir"];
        if (dir.eoo() || !dir.isNumber()) {
            return {ErrorCodes::Error{5619227},
                    "found a collection scan in the solution but no numeric 'dir' in the provided "
                    "JSON"};
        }
        if (dir.numberInt() != csn->direction) {
            return {ErrorCodes::Error{5619228},
                    str::stream() << "Solution does not match: found a collection scan in "
                                     "the solution but in the wrong direction. Found "
                                  << csn->direction << " but was expecting " << dir.numberInt()};
        }

        BSONElement filter = csObj["filter"];
        if (filter.eoo()) {
            LOGV2(3155103,
                  "Found a collection scan which was expected. No filter provided to check");
            return Status::OK();
        } else if (filter.isNull()) {
            if (csn->filter == nullptr) {
                return Status::OK();
            }
            return {
                ErrorCodes::Error{5619209},
                str::stream() << "Expected a collection scan without a filter, but found a filter: "
                              << csn->filter->toString()};
        } else if (!filter.isABSONObj()) {
            return {ErrorCodes::Error{5619229},
                    str::stream() << "Provided JSON gave a 'cscan' with a 'filter', but the filter "
                                     "was not an object."
                                  << filter};
        }

        BSONObj collation;
        if (BSONElement collationElt = csObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return {ErrorCodes::Error{5619230},
                        str::stream()
                            << "Provided JSON gave a 'cscan' with a 'collation', but the collation "
                               "was not an object:"
                            << collationElt};
            }
            collation = collationElt.Obj();
        }

        return nodeHasMatchingFilter(filter.Obj(), collation, trueSoln)
            .withContext("mismatching 'filter' for 'cscan' node");
    } else if (STAGE_IXSCAN == trueSoln->getType()) {
        const IndexScanNode* ixn = static_cast<const IndexScanNode*>(trueSoln);
        BSONElement el = testSoln["ixscan"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619231},
                    "found an index scan in the solution but no corresponding 'ixscan' object in "
                    "the provided JSON"};
        }
        BSONObj ixscanObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(
            ixscanObj, {"pattern", "name", "bounds", "dir", "filter", "collation"}));

        BSONElement pattern = ixscanObj["pattern"];
        if (!pattern.eoo()) {
            if (!pattern.isABSONObj()) {
                return {ErrorCodes::Error{5619232},
                        str::stream()
                            << "Provided JSON gave a 'ixscan' with a 'pattern', but the pattern "
                               "was not an object: "
                            << pattern};
            }
            if (SimpleBSONObjComparator::kInstance.evaluate(pattern.Obj() !=
                                                            ixn->index.keyPattern)) {
                return {ErrorCodes::Error{5619233},
                        str::stream() << "Provided JSON gave a 'ixscan' with a 'pattern' which did "
                                         "not match. Expected: "
                                      << pattern.Obj() << " Found: " << ixn->index.keyPattern};
            }
        }

        BSONElement name = ixscanObj["name"];
        if (!name.eoo()) {
            if (name.type() != BSONType::String) {
                return {ErrorCodes::Error{5619234},
                        str::stream()
                            << "Provided JSON gave a 'ixscan' with a 'name', but the name "
                               "was not an string: "
                            << name};
            }
            if (name.valueStringData() != ixn->index.identifier.catalogName) {
                return {ErrorCodes::Error{5619235},
                        str::stream() << "Provided JSON gave a 'ixscan' with a 'name' which did "
                                         "not match. Expected: "
                                      << name << " Found: " << ixn->index.identifier.catalogName};
            }
        }

        if (name.eoo() && pattern.eoo()) {
            return {ErrorCodes::Error{5619236},
                    "Provided JSON gave a 'ixscan' without a 'name' or a 'pattern.'"};
        }

        BSONElement bounds = ixscanObj["bounds"];
        if (!bounds.eoo()) {
            if (!bounds.isABSONObj()) {
                return {ErrorCodes::Error{5619237},
                        str::stream()
                            << "Provided JSON gave a 'ixscan' with a 'bounds', but the bounds "
                               "was not an object: "
                            << bounds};
            } else if (auto boundsStatus = boundsMatch(bounds.Obj(), ixn->bounds, relaxBoundsCheck);
                       !boundsStatus.isOK()) {
                return boundsStatus.withContext(
                    "Provided JSON gave a 'ixscan' with 'bounds' which did not match");
            }
        }

        BSONElement dir = ixscanObj["dir"];
        if (!dir.eoo() && dir.isNumber()) {
            if (dir.numberInt() != ixn->direction) {
                return {ErrorCodes::Error{5619238},
                        str::stream()
                            << "Solution does not match: found an index scan in "
                               "the solution but in the wrong direction. Found "
                            << ixn->direction << " but was expecting " << dir.numberInt()};
            }
        }

        BSONElement filter = ixscanObj["filter"];
        if (filter.eoo()) {
            return Status::OK();
        } else if (filter.isNull()) {
            if (ixn->filter == nullptr) {
                return Status::OK();
            }
            return {ErrorCodes::Error{5619239},
                    str::stream() << "Expected an index scan without a filter, but found a filter: "
                                  << ixn->filter->toString()};
        } else if (!filter.isABSONObj()) {
            return {
                ErrorCodes::Error{5619240},
                str::stream() << "Provided JSON gave an 'ixscan' with a 'filter', but the filter "
                                 "was not an object: "
                              << filter};
        }

        BSONObj collation;
        if (BSONElement collationElt = ixscanObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return {
                    ErrorCodes::Error{5619241},
                    str::stream()
                        << "Provided JSON gave an 'ixscan' with a 'collation', but the collation "
                           "was not an object:"
                        << collationElt};
            }
            collation = collationElt.Obj();
        }

        return nodeHasMatchingFilter(filter.Obj(), collation, trueSoln)
            .withContext("mismatching 'filter' for 'ixscan' node");
    } else if (STAGE_GEO_NEAR_2D == trueSoln->getType()) {
        const GeoNear2DNode* node = static_cast<const GeoNear2DNode*>(trueSoln);
        BSONElement el = testSoln["geoNear2d"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619242},
                    "found a geoNear2d stage in the solution but no "
                    "corresponding 'geoNear2d' object in the provided JSON"};
        }
        BSONObj geoObj = el.Obj();
        if (SimpleBSONObjComparator::kInstance.evaluate(geoObj == node->index.keyPattern)) {
            return Status::OK();
        }
        return {
            ErrorCodes::Error{5619243},
            str::stream()
                << "found a geoNear2d stage in the solution with mismatching keyPattern. Expected: "
                << geoObj << " Found: " << node->index.keyPattern};
    } else if (STAGE_GEO_NEAR_2DSPHERE == trueSoln->getType()) {
        const GeoNear2DSphereNode* node = static_cast<const GeoNear2DSphereNode*>(trueSoln);
        BSONElement el = testSoln["geoNear2dsphere"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619244},
                    "found a geoNear2dsphere stage in the solution but no "
                    "corresponding 'geoNear2dsphere' object in the provided JSON"};
        }
        BSONObj geoObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(geoObj, {"pattern", "bounds"}));

        BSONElement pattern = geoObj["pattern"];
        if (pattern.eoo() || !pattern.isABSONObj()) {
            return {ErrorCodes::Error{5619245},
                    "found a geoNear2dsphere stage in the solution but no 'pattern' object "
                    "in the provided JSON"};
        }
        if (SimpleBSONObjComparator::kInstance.evaluate(pattern.Obj() != node->index.keyPattern)) {
            return {ErrorCodes::Error{5619246},
                    str::stream() << "found a geoNear2dsphere stage in the solution with "
                                     "mismatching keyPattern. Expected: "
                                  << pattern.Obj() << " Found: " << node->index.keyPattern};
        }

        BSONElement bounds = geoObj["bounds"];
        if (!bounds.eoo()) {
            if (!bounds.isABSONObj()) {
                return {
                    ErrorCodes::Error{5619247},
                    str::stream()
                        << "Provided JSON gave a 'geoNear2dsphere' with a 'bounds', but the bounds "
                           "was not an object: "
                        << bounds};
            } else if (auto boundsStatus =
                           boundsMatch(bounds.Obj(), node->baseBounds, relaxBoundsCheck);
                       !boundsStatus.isOK()) {
                return boundsStatus.withContext(
                    "Provided JSON gave a 'geoNear2dsphere' with 'bounds' which did not match");
            }
        }

        return Status::OK();
    } else if (STAGE_TEXT_MATCH == trueSoln->getType()) {
        // {text: {search: "somestr", language: "something", filter: {blah: 1}}}
        const TextMatchNode* node = static_cast<const TextMatchNode*>(trueSoln);
        BSONElement el = testSoln["text"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619248},
                    "found a text stage in the solution but no "
                    "corresponding 'text' object in the provided JSON"};
        }
        BSONObj textObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(textObj,
                                        {"text",
                                         "search",
                                         "language",
                                         "caseSensitive",
                                         "diacriticSensitive",
                                         "prefix",
                                         "collation",
                                         "filter"}));

        BSONElement searchElt = textObj["search"];
        if (!searchElt.eoo()) {
            if (searchElt.String() != node->ftsQuery->getQuery()) {
                return {ErrorCodes::Error{5619249},
                        str::stream()
                            << "found a text stage in the solution with "
                               "mismatching 'search'. Expected: "
                            << searchElt.String() << " Found: " << node->ftsQuery->getQuery()};
            }
        }

        BSONElement languageElt = textObj["language"];
        if (!languageElt.eoo()) {
            if (languageElt.String() != node->ftsQuery->getLanguage()) {
                return {ErrorCodes::Error{5619250},
                        str::stream()
                            << "found a text stage in the solution with "
                               "mismatching 'language'. Expected: "
                            << languageElt.String() << " Found: " << node->ftsQuery->getLanguage()};
            }
        }

        BSONElement caseSensitiveElt = textObj["caseSensitive"];
        if (!caseSensitiveElt.eoo()) {
            if (caseSensitiveElt.trueValue() != node->ftsQuery->getCaseSensitive()) {
                return {ErrorCodes::Error{5619251},
                        str::stream() << "found a text stage in the solution with "
                                         "mismatching 'caseSensitive'. Expected: "
                                      << caseSensitiveElt.trueValue()
                                      << " Found: " << node->ftsQuery->getCaseSensitive()};
            }
        }

        BSONElement diacriticSensitiveElt = textObj["diacriticSensitive"];
        if (!diacriticSensitiveElt.eoo()) {
            if (diacriticSensitiveElt.trueValue() != node->ftsQuery->getDiacriticSensitive()) {
                return {ErrorCodes::Error{5619252},
                        str::stream() << "found a text stage in the solution with "
                                         "mismatching 'diacriticSensitive'. Expected: "
                                      << diacriticSensitiveElt.trueValue()
                                      << " Found: " << node->ftsQuery->getDiacriticSensitive()};
            }
        }

        BSONElement indexPrefix = textObj["prefix"];
        if (!indexPrefix.eoo()) {
            if (!indexPrefix.isABSONObj()) {
                return {ErrorCodes::Error{5619253},
                        str::stream()
                            << "Provided JSON gave a 'text' with a 'prefix', but the prefix "
                               "was not an object: "
                            << indexPrefix};
            }

            if (0 != indexPrefix.Obj().woCompare(node->indexPrefix)) {
                return {ErrorCodes::Error{5619254},
                        str::stream() << "found a text stage in the solution with "
                                         "mismatching 'prefix'. Expected: "
                                      << indexPrefix.Obj() << " Found: " << node->indexPrefix};
            }
        }

        BSONObj collation;
        if (BSONElement collationElt = textObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return {ErrorCodes::Error{5619255},
                        str::stream() << "Provided JSON gave a 'text' stage with a 'collation', "
                                         "but the collation was not an object:"
                                      << collationElt};
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = textObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (nullptr != node->filter) {
                    return {ErrorCodes::Error{5619256},
                            str::stream()
                                << "Expected a text stage without a filter, but found a filter: "
                                << node->filter->toString()};
                }
            } else if (!filter.isABSONObj()) {
                return {ErrorCodes::Error{5619257},
                        str::stream()
                            << "Provided JSON gave a 'text' stage with a 'filter', but the filter "
                               "was not an object."
                            << filter};
            } else {
                return nodeHasMatchingFilter(filter.Obj(), collation, trueSoln)
                    .withContext("mismatching 'filter' for 'text' node");
            }
        }

        return Status::OK();
    }

    //
    // internal nodes
    //
    if (STAGE_FETCH == trueSoln->getType()) {
        const FetchNode* fn = static_cast<const FetchNode*>(trueSoln);

        BSONElement el = testSoln["fetch"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619258},
                    "found a fetch in the solution but no corresponding 'fetch' object in "
                    "the provided JSON"};
        }
        BSONObj fetchObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(fetchObj, {"collation", "filter", "node"}));

        BSONObj collation;
        if (BSONElement collationElt = fetchObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return {ErrorCodes::Error{5619259},
                        str::stream()
                            << "Provided JSON gave a 'fetch' with a 'collation', but the collation "
                               "was not an object:"
                            << collationElt};
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = fetchObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (nullptr != fn->filter) {
                    return {ErrorCodes::Error{5619260},
                            str::stream()
                                << "Expected a fetch stage without a filter, but found a filter: "
                                << fn->filter->toString()};
                }
            } else if (!filter.isABSONObj()) {
                return {ErrorCodes::Error{5619261},
                        str::stream()
                            << "Provided JSON gave a 'fetch' stage with a 'filter', but the filter "
                               "was not an object."
                            << filter};
            } else if (auto filterStatus = nodeHasMatchingFilter(filter.Obj(), collation, trueSoln);
                       !filterStatus.isOK()) {
                return filterStatus.withContext("mismatching 'filter' for 'fetch' node");
            }
        }

        BSONElement child = fetchObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return {ErrorCodes::Error{5619262},
                    "found a fetch stage in the solution but no 'node' sub-object in the provided "
                    "JSON"};
        }
        return solutionMatches(child.Obj(), fn->children[0].get(), relaxBoundsCheck)
            .withContext("mismatch beneath fetch node");
    } else if (STAGE_OR == trueSoln->getType()) {
        const OrNode* orn = static_cast<const OrNode*>(trueSoln);
        BSONElement el = testSoln["or"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619263},
                    "found an OR stage in the solution but no "
                    "corresponding 'or' object in the provided JSON"};
        }
        BSONObj orObj = el.Obj();
        return childrenMatch(orObj, orn, relaxBoundsCheck).withContext("mismatch beneath or node");
    } else if (STAGE_AND_HASH == trueSoln->getType()) {
        const AndHashNode* ahn = static_cast<const AndHashNode*>(trueSoln);
        BSONElement el = testSoln["andHash"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619264},
                    "found an AND_HASH stage in the solution but no "
                    "corresponding 'andHash' object in the provided JSON"};
        }
        BSONObj andHashObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(andHashObj, {"collation", "filter", "nodes"}));

        BSONObj collation;
        if (BSONElement collationElt = andHashObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return {ErrorCodes::Error{5619265},
                        str::stream()
                            << "Provided JSON gave an 'andHash' stage with a 'collation', "
                               "but the collation was not an object:"
                            << collationElt};
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = andHashObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (nullptr != ahn->filter) {
                    return {
                        ErrorCodes::Error{5619266},
                        str::stream()
                            << "Expected an AND_HASH stage without a filter, but found a filter: "
                            << ahn->filter->toString()};
                }
            } else if (!filter.isABSONObj()) {
                return {
                    ErrorCodes::Error{5619267},
                    str::stream()
                        << "Provided JSON gave an AND_HASH stage with a 'filter', but the filter "
                           "was not an object."
                        << filter};
            } else if (auto matchStatus = nodeHasMatchingFilter(filter.Obj(), collation, trueSoln);
                       !matchStatus.isOK()) {
                return matchStatus.withContext("mismatching 'filter' for AND_HASH node");
            }
        }

        return childrenMatch(andHashObj, ahn, relaxBoundsCheck)
            .withContext("mismatching children beneath AND_HASH node");
    } else if (STAGE_AND_SORTED == trueSoln->getType()) {
        const AndSortedNode* asn = static_cast<const AndSortedNode*>(trueSoln);
        BSONElement el = testSoln["andSorted"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619268},
                    "found an AND_SORTED stage in the solution but no "
                    "corresponding 'andSorted' object in the provided JSON"};
        }
        BSONObj andSortedObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(andSortedObj, {"collation", "filter", "nodes"}));

        BSONObj collation;
        if (BSONElement collationElt = andSortedObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return {ErrorCodes::Error{5619269},
                        str::stream()
                            << "Provided JSON gave an 'andSorted' stage with a 'collation', "
                               "but the collation was not an object:"
                            << collationElt};
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = andSortedObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (nullptr != asn->filter) {
                    return {
                        ErrorCodes::Error{5619270},
                        str::stream()
                            << "Expected an AND_SORTED stage without a filter, but found a filter: "
                            << asn->filter->toString()};
                }
            } else if (!filter.isABSONObj()) {
                return {
                    ErrorCodes::Error{5619271},
                    str::stream()
                        << "Provided JSON gave an AND_SORTED stage with a 'filter', but the filter "
                           "was not an object."
                        << filter};
            } else if (auto matchStatus = nodeHasMatchingFilter(filter.Obj(), collation, trueSoln);
                       !matchStatus.isOK()) {
                return matchStatus.withContext("mismatching 'filter' for AND_SORTED node");
            }
        }

        return childrenMatch(andSortedObj, asn, relaxBoundsCheck)
            .withContext("mismatching children beneath AND_SORTED node");
    } else if (isProjectionStageType(trueSoln->getType())) {
        const ProjectionNode* pn = static_cast<const ProjectionNode*>(trueSoln);

        BSONElement el = testSoln["proj"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619272},
                    "found a projection stage in the solution but no "
                    "corresponding 'proj' object in the provided JSON"};
        }
        BSONObj projObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(projObj, {"type", "spec", "node"}));

        BSONElement projType = projObj["type"];
        if (!projType.eoo()) {
            string projTypeStr = projType.str();
            switch (pn->getType()) {
                case StageType::STAGE_PROJECTION_DEFAULT:
                    if (projTypeStr != "default")
                        return {ErrorCodes::Error{5619273},
                                str::stream() << "found a projection stage in the solution with "
                                                 "mismatching 'type'. Expected: "
                                              << projTypeStr << " Found: 'default'"};
                    break;
                case StageType::STAGE_PROJECTION_COVERED:
                    if (projTypeStr != "coveredIndex")
                        return {ErrorCodes::Error{5619274},
                                str::stream() << "found a projection stage in the solution with "
                                                 "mismatching 'type'. Expected: "
                                              << projTypeStr << " Found: 'coveredIndex'"};
                    break;
                case StageType::STAGE_PROJECTION_SIMPLE:
                    if (projTypeStr != "simple")
                        return {ErrorCodes::Error{5619275},
                                str::stream() << "found a projection stage in the solution with "
                                                 "mismatching 'type'. Expected: "
                                              << projTypeStr << " Found: 'simple'"};
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }

        BSONElement spec = projObj["spec"];
        if (spec.eoo() || !spec.isABSONObj()) {
            return {ErrorCodes::Error{5619276},
                    "found a projection stage in the solution but no 'spec' object in the provided "
                    "JSON"};
        }
        BSONElement child = projObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return {
                ErrorCodes::Error{5619277},
                "found a projection stage in the solution but no 'node' sub-object in the provided "
                "JSON"};
        }

        // Create an empty/dummy expression context without access to the operation context and
        // collator. This should be sufficient to parse a projection.
        auto expCtx =
            make_intrusive<ExpressionContext>(nullptr, nullptr, NamespaceString("test.dummy"));
        auto projection = projection_ast::parseAndAnalyze(
            expCtx, spec.Obj(), ProjectionPolicies::findProjectionPolicies());
        auto specProjObj = projection_ast::astToDebugBSON(projection.root());
        auto solnProjObj = projection_ast::astToDebugBSON(pn->proj.root());
        if (!SimpleBSONObjComparator::kInstance.evaluate(specProjObj == solnProjObj)) {
            return {ErrorCodes::Error{5619278},
                    str::stream() << "found a projection stage in the solution with "
                                     "mismatching 'spec'. Expected: "
                                  << specProjObj << " Found: " << solnProjObj};
        }
        return solutionMatches(child.Obj(), pn->children[0].get(), relaxBoundsCheck)
            .withContext("mismatch below projection stage");
    } else if (isSortStageType(trueSoln->getType())) {
        const SortNode* sn = static_cast<const SortNode*>(trueSoln);
        BSONElement el = testSoln["sort"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619279},
                    "found a sort stage in the solution but no "
                    "corresponding 'sort' object in the provided JSON"};
        }
        BSONObj sortObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(sortObj, {"pattern", "limit", "type", "node"}));

        BSONElement patternEl = sortObj["pattern"];
        if (patternEl.eoo() || !patternEl.isABSONObj()) {
            return {
                ErrorCodes::Error{5619280},
                "found a sort stage in the solution but no 'pattern' object in the provided JSON"};
        }
        BSONElement limitEl = sortObj["limit"];
        if (limitEl.eoo()) {
            return {ErrorCodes::Error{5619281},
                    "found a sort stage in the solution but no 'limit' was provided. Specify '0' "
                    "for no limit."};
        }
        if (!limitEl.isNumber()) {
            return {
                ErrorCodes::Error{5619282},
                str::stream() << "found a sort stage in the solution but 'limit' was not numeric: "
                              << limitEl};
        }

        BSONElement sortType = sortObj["type"];
        if (sortType) {
            if (sortType.type() != BSONType::String) {
                return {ErrorCodes::Error{5619283},
                        str::stream()
                            << "found a sort stage in the solution but 'type' was not a string: "
                            << sortType};
            }

            auto sortTypeString = sortType.valueStringData();
            switch (sn->getType()) {
                case StageType::STAGE_SORT_DEFAULT: {
                    if (sortTypeString != "default") {
                        return {ErrorCodes::Error{5619284},
                                str::stream() << "found a sort stage in the solution with "
                                                 "mismatching 'type'. Expected: "
                                              << sortTypeString << " Found: 'default'"};
                    }
                    break;
                }
                case StageType::STAGE_SORT_SIMPLE: {
                    if (sortTypeString != "simple") {
                        return {ErrorCodes::Error{5619285},
                                str::stream() << "found a sort stage in the solution with "
                                                 "mismatching 'type'. Expected: "
                                              << sortTypeString << " Found: 'simple'"};
                    }
                    break;
                }
                default: { MONGO_UNREACHABLE; }
            }
        }

        BSONElement child = sortObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return {
                ErrorCodes::Error{5619286},
                "found a sort stage in the solution but no 'node' sub-object in the provided JSON"};
        }

        size_t expectedLimit = limitEl.numberInt();
        if (!SimpleBSONObjComparator::kInstance.evaluate(patternEl.Obj() == sn->pattern)) {
            return {ErrorCodes::Error{5619287},
                    str::stream() << "found a sort stage in the solution with "
                                     "mismatching 'pattern'. Expected: "
                                  << patternEl << " Found: " << sn->pattern};
        }
        if (expectedLimit != sn->limit) {
            return {ErrorCodes::Error{5619288},
                    str::stream() << "found a projection stage in the solution with "
                                     "mismatching 'limit'. Expected: "
                                  << expectedLimit << " Found: " << sn->limit};
        }
        return solutionMatches(child.Obj(), sn->children[0].get(), relaxBoundsCheck)
            .withContext("mismatch below sort stage");
    } else if (STAGE_SORT_KEY_GENERATOR == trueSoln->getType()) {
        const SortKeyGeneratorNode* keyGenNode = static_cast<const SortKeyGeneratorNode*>(trueSoln);
        BSONElement el = testSoln["sortKeyGen"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619289},
                    "found a sort key generator stage in the solution but no "
                    "corresponding 'sortKeyGen' object in the provided JSON"};
        }
        BSONObj keyGenObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(keyGenObj, {"node"}));

        BSONElement child = keyGenObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return {ErrorCodes::Error{5619290},
                    "found a sort key generator stage in the solution but no 'node' sub-object in "
                    "the provided JSON"};
        }

        return solutionMatches(child.Obj(), keyGenNode->children[0].get(), relaxBoundsCheck)
            .withContext("mismatch below sortKeyGen");
    } else if (STAGE_SORT_MERGE == trueSoln->getType()) {
        const MergeSortNode* msn = static_cast<const MergeSortNode*>(trueSoln);
        BSONElement el = testSoln["mergeSort"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619291},
                    "found a merge sort stage in the solution but no "
                    "corresponding 'mergeSort' object in the provided JSON"};
        }
        BSONObj mergeSortObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(mergeSortObj, {"nodes"}));
        return childrenMatch(mergeSortObj, msn, relaxBoundsCheck)
            .withContext("mismatching children below merge sort");
    } else if (STAGE_SKIP == trueSoln->getType()) {
        const SkipNode* sn = static_cast<const SkipNode*>(trueSoln);
        BSONElement el = testSoln["skip"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619292},
                    "found a skip stage in the solution but no "
                    "corresponding 'skip' object in the provided JSON"};
        }
        BSONObj skipObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(skipObj, {"n", "node"}));

        BSONElement skipEl = skipObj["n"];
        if (!skipEl.isNumber()) {
            return {ErrorCodes::Error{5619293},
                    str::stream() << "found a skip stage in the solution but 'n' was not numeric: "
                                  << skipEl};
        }
        BSONElement child = skipObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return {ErrorCodes::Error{5619294},
                    "found a skip stage in the solution but no 'node' sub-object in "
                    "the provided JSON"};
        }

        if (skipEl.numberInt() != sn->skip) {
            return {ErrorCodes::Error{5619295},
                    str::stream() << "found a skip stage in the solution with "
                                     "mismatching 'n'. Expected: "
                                  << skipEl.numberInt() << " Found: " << sn->skip};
        }
        return solutionMatches(child.Obj(), sn->children[0].get(), relaxBoundsCheck)
            .withContext("mismatch below skip stage");
    } else if (STAGE_LIMIT == trueSoln->getType()) {
        const LimitNode* ln = static_cast<const LimitNode*>(trueSoln);
        BSONElement el = testSoln["limit"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619296},
                    "found a limit stage in the solution but no "
                    "corresponding 'limit' object in the provided JSON"};
        }
        BSONObj limitObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(limitObj, {"n", "node"}));

        BSONElement limitEl = limitObj["n"];
        if (!limitEl.isNumber()) {
            return {ErrorCodes::Error{5619297},
                    str::stream() << "found a limit stage in the solution but 'n' was not numeric: "
                                  << limitEl};
        }
        BSONElement child = limitObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return {ErrorCodes::Error{5619298},
                    "found a limit stage in the solution but no 'node' sub-object in "
                    "the provided JSON"};
        }

        if (limitEl.numberInt() != ln->limit) {
            return {ErrorCodes::Error{5619299},
                    str::stream() << "found a limit stage in the solution with "
                                     "mismatching 'n'. Expected: "
                                  << limitEl.numberInt() << " Found: " << ln->limit};
        }
        return solutionMatches(child.Obj(), ln->children[0].get(), relaxBoundsCheck)
            .withContext("mismatch below limit stage");
    } else if (STAGE_SHARDING_FILTER == trueSoln->getType()) {
        const ShardingFilterNode* fn = static_cast<const ShardingFilterNode*>(trueSoln);

        BSONElement el = testSoln["sharding_filter"];
        if (el.eoo() || !el.isABSONObj()) {
            return {ErrorCodes::Error{5619206},
                    "found a sharding filter stage in the solution but no "
                    "corresponding 'sharding_filter' object in the provided JSON"};
        }
        BSONObj keepObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(keepObj, {"node"}));

        BSONElement child = keepObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return {ErrorCodes::Error{5619207},
                    "found a sharding filter stage in the solution but no 'node' sub-object in "
                    "the provided JSON"};
        }

        return solutionMatches(child.Obj(), fn->children[0].get(), relaxBoundsCheck)
            .withContext("mismatch below shard filter stage");
    } else if (STAGE_GROUP == trueSoln->getType()) {
        const auto* actualGroupNode = static_cast<const GroupNode*>(trueSoln);
        auto expectedGroupElem = testSoln["group"];
        if (expectedGroupElem.eoo() || !expectedGroupElem.isABSONObj()) {
            return {ErrorCodes::Error{5842401},
                    "found a 'group' object in the test solution but no corresponding 'group' "
                    "object "
                    "in the expected JSON"};
        }

        auto expectedGroupObj = expectedGroupElem.Obj();
        invariant(bsonObjFieldsAreInSet(expectedGroupObj, {"key", "accs", "node"}));

        auto expectedGroupByElem = expectedGroupObj["key"];
        if (expectedGroupByElem.eoo() || !expectedGroupByElem.isABSONObj()) {
            return {ErrorCodes::Error{5842402},
                    "found a 'key' object in the test solution but no corresponding 'key' "
                    "object "
                    "in the expected JSON"};
        }

        BSONObjBuilder bob;
        actualGroupNode->groupByExpression->serialize(true).addToBsonObj(&bob, "_id");
        auto actualGroupByObj = bob.done();
        if (!SimpleBSONObjComparator::kInstance.evaluate(actualGroupByObj ==
                                                         expectedGroupByElem.Obj())) {
            return {ErrorCodes::Error{5842403},
                    str::stream() << "found a group stage in the solution with "
                                     "mismatching 'key' expressions. Expected: "
                                  << expectedGroupByElem.Obj() << " Found: " << actualGroupByObj};
        }

        BSONArrayBuilder actualAccs;
        for (auto& acc : actualGroupNode->accumulators) {
            BSONObjBuilder bob;
            acc.expr.argument->serialize(true).addToBsonObj(&bob, acc.expr.name);
            actualAccs.append(BSON(acc.fieldName << bob.done()));
        }
        auto expectedAccsObj = expectedGroupObj["accs"].Obj();
        auto actualAccsObj = actualAccs.done();
        if (!SimpleBSONObjComparator::kInstance.evaluate(expectedAccsObj == actualAccsObj)) {
            return {ErrorCodes::Error{5842404},
                    str::stream() << "found a group stage in the solution with "
                                     "mismatching 'accs' expressions. Expected: "
                                  << expectedAccsObj << " Found: " << actualAccsObj};
        }

        auto child = expectedGroupObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return {ErrorCodes::Error{5842405},
                    "found a group stage in the solution but no 'node' sub-object in "
                    "the provided JSON"};
        }
        return solutionMatches(child.Obj(), actualGroupNode->children[0].get(), relaxBoundsCheck)
            .withContext("mismatch below group stage");
    } else if (STAGE_SENTINEL == trueSoln->getType()) {
        const auto* actualSentinelNode = static_cast<const SentinelNode*>(trueSoln);
        auto expectedSentinelElem = testSoln["sentinel"];
        if (expectedSentinelElem.eoo() || !expectedSentinelElem.isABSONObj()) {
            return {
                ErrorCodes::Error{5842406},
                "found a 'sentinel' object in the test solution but no corresponding 'sentinel' "
                "object "
                "in the expected JSON"};
        }
        // The sentinel node is just an empty QSN node.
        if (!expectedSentinelElem.Obj().isEmpty()) {
            return {ErrorCodes::Error{5842407},
                    str::stream() << "found a non-empty sentinel stage in the solution"};
        }
        if (!actualSentinelNode->children.empty()) {
            return {ErrorCodes::Error{5842408},
                    str::stream() << "found a sentinel stage with more than zero children in the "
                                     "actual solution. Expected: "
                                  << testSoln << " Found: " << actualSentinelNode};
        }
        return Status::OK();
    } else if (STAGE_COLUMN_SCAN == trueSoln->getType()) {
        const auto* actualColumnIxScanNode = static_cast<const ColumnIndexScanNode*>(trueSoln);
        auto expectedElem = testSoln["column_scan"];
        if (expectedElem.eoo() || !expectedElem.isABSONObj()) {
            return {ErrorCodes::Error{5842490},
                    "found a 'column_scan' object in the test solution but no corresponding "
                    "'column_scan' object in the expected JSON"};
        }
        auto obj = expectedElem.Obj();

        if (auto outputFields = obj["outputFields"]) {
            if (auto outputStatus =
                    stringSetsMatch(outputFields,
                                    actualColumnIxScanNode->outputFields,
                                    "mismatching output fields within 'column_scan'");
                !outputStatus.isOK()) {
                return outputStatus;
            }
        }

        if (auto matchFields = obj["matchFields"]) {
            if (auto matchStatus = stringSetsMatch(matchFields,
                                                   actualColumnIxScanNode->matchFields,
                                                   "mismatching match fields within 'column_scan'");
                !matchStatus.isOK()) {
                return matchStatus;
            }
        }

        if (!actualColumnIxScanNode->children.empty()) {
            return {
                ErrorCodes::Error{5842492},
                "found a column_scan stage with more than zero children in the actual solution:"};
        }

        // All QuerySolutionNodes can have a 'filter' option, but the column index stage is special.
        // Make sure the caller doesn't expect this and that the actual solution doesn't store
        // anything in that field either.
        if (auto filter = obj["filter"]) {
            return {ErrorCodes::Error{6312402},
                    "do not specify 'filter' to a 'column_scan', specify 'filtersByPath' instead"};
        }
        if (actualColumnIxScanNode->filter) {
            return {ErrorCodes::Error{6312403},
                    "'column_scan' solution node found with a non-empty 'filter'. We expect this "
                    "to be null and 'filtersByPath' to be used instead."};
        }

        if (auto filtersByPath = obj["filtersByPath"]) {
            if (filtersByPath.type() != BSONType::Object) {
                return {ErrorCodes::Error{6412404},
                        str::stream() << "invalid 'filtersByPath' specified to 'column_scan' "
                                         "stage. Please specify an object. Found: "
                                      << filtersByPath};
            }

            const auto expectedFiltersByPath = filtersByPath.Obj();
            if (auto filtersMatchStatus = columnIxScanFiltersByPathMatch(
                    expectedFiltersByPath, actualColumnIxScanNode->filtersByPath);
                !filtersMatchStatus.isOK()) {
                return filtersMatchStatus.withContext("mismatching filters in 'column_scan' stage");
            }
        }

        if (auto postAssemblyFilter = obj["postAssemblyFilter"]) {
            if (postAssemblyFilter.type() != BSONType::Object) {
                return {ErrorCodes::Error{6412408},
                        str::stream() << "invalid 'postAssemblyFilter' specified to 'column_scan' "
                                         "stage. Please specify an object. Found: "
                                      << postAssemblyFilter};
            }

            const auto expectedPostAssemblyFilter = postAssemblyFilter.Obj();
            if (auto filtersMatchStatus =
                    filterMatches(expectedPostAssemblyFilter,
                                  actualColumnIxScanNode->postAssemblyFilter.get(),
                                  nullptr);
                !filtersMatchStatus.isOK()) {
                return filtersMatchStatus.withContext(
                    "mismatching 'postAssemblyFilter' in 'column_scan' stage");
            }
        }

        return Status::OK();
    } else if (STAGE_EQ_LOOKUP == trueSoln->getType()) {
        const auto* actualEqLookupNode = static_cast<const EqLookupNode*>(trueSoln);
        auto expectedElem = testSoln["eq_lookup"];
        if (expectedElem.eoo() || !expectedElem.isABSONObj()) {
            return {ErrorCodes::Error{6267500},
                    "found a 'eq_lookup' object in the test solution but no corresponding "
                    "'eq_lookup' object in the expected JSON"};
        }

        auto expectedEqLookupSoln = expectedElem.Obj();
        auto expectedForeignCollection = expectedEqLookupSoln["foreignCollection"];
        if (expectedForeignCollection.eoo() ||
            expectedForeignCollection.type() != BSONType::String) {
            return {ErrorCodes::Error{6267501},
                    str::stream() << "Test solution for eq_lookup should have a "
                                     "'foreignCollection' field that is "
                                     "a string; "
                                  << testSoln.toString()};
        }

        if (expectedForeignCollection.str() != actualEqLookupNode->foreignCollection.toString()) {
            return {
                ErrorCodes::Error{6267502},
                str::stream() << "Test solution 'foreignCollection' does not match actual; test "
                                 ""
                              << expectedForeignCollection.str() << " != actual "
                              << actualEqLookupNode->foreignCollection};
        }

        auto expectedLocalField = expectedEqLookupSoln["joinFieldLocal"];
        if (expectedLocalField.eoo() || expectedLocalField.type() != BSONType::String) {
            return {
                ErrorCodes::Error{6267503},
                str::stream()
                    << "Test solution for eq_lookup should have a 'joinFieldLocal' field that is "
                       "a string; "
                    << testSoln.toString()};
        }


        if (expectedLocalField.str() != actualEqLookupNode->joinFieldLocal.fullPath()) {
            return {ErrorCodes::Error{6267504},
                    str::stream() << "Test solution 'joinFieldLocal' does not match actual; test "
                                     ""
                                  << expectedLocalField.str() << " != actual "
                                  << actualEqLookupNode->joinFieldLocal.fullPath()};
        }

        auto expectedForeignField = expectedEqLookupSoln["joinFieldForeign"];
        if (expectedForeignField.eoo() || expectedForeignField.type() != BSONType::String) {
            return {
                ErrorCodes::Error{6267505},
                str::stream()
                    << "Test solution for eq_lookup should have a 'joinFieldForeign' field that is "
                       "a string; "
                    << testSoln.toString()};
        }

        if (expectedForeignField.str() != actualEqLookupNode->joinFieldForeign.fullPath()) {
            return {ErrorCodes::Error{6267506},
                    str::stream() << "Test solution 'joinFieldForeign' does not match actual; test "
                                     ""
                                  << expectedForeignField.str() << " != actual "
                                  << actualEqLookupNode->joinFieldForeign.fullPath()};
        }

        auto expectedAsField = expectedEqLookupSoln["joinField"];
        if (expectedAsField.eoo() || expectedAsField.type() != BSONType::String) {
            return {ErrorCodes::Error{6267507},
                    str::stream()
                        << "Test solution for eq_lookup should have a 'joinField' field that is "
                           "a string; "
                        << testSoln.toString()};
        }

        if (expectedAsField.str() != actualEqLookupNode->joinField.fullPath()) {
            return {ErrorCodes::Error{6267508},
                    str::stream() << "Test solution 'joinField' does not match actual; test "
                                     ""
                                  << expectedAsField.str() << " != actual "
                                  << actualEqLookupNode->joinField.fullPath()};
        }

        auto expectedStrategy = expectedEqLookupSoln["strategy"];
        if (expectedStrategy.eoo() || expectedStrategy.type() != BSONType::String) {
            return {ErrorCodes::Error{6267509},
                    str::stream()
                        << "Test solution for eq_lookup should have a 'strategy' field that is "
                           "a string; "
                        << testSoln.toString()};
        }

        auto actualLookupStrategy =
            EqLookupNode::serializeLookupStrategy(actualEqLookupNode->lookupStrategy);
        if (expectedStrategy.str() != actualLookupStrategy) {
            return {ErrorCodes::Error{6267510},
                    str::stream() << "Test solution 'expectedStrategy' does not match actual; test "
                                  << expectedStrategy.str() << " != actual "
                                  << actualLookupStrategy};
        }

        auto child = expectedEqLookupSoln["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return {ErrorCodes::Error{6267511},
                    "found a eq_lookup stage in the solution but no 'node' sub-object in "
                    "the provided JSON"};
        }
        return solutionMatches(child.Obj(), actualEqLookupNode->children[0].get(), relaxBoundsCheck)
            .withContext("mismatch below eq_lookup stage");
    }
    return {ErrorCodes::Error{5698301},
            str::stream() << "Unknown query solution node found: " << trueSoln->toString()};
}  // namespace mongo

}  // namespace mongo
