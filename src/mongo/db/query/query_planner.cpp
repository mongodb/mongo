/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/query_planner.h"

#include <map>
#include <set>
#include <stack>
#include <vector>

// For QueryOption_foobar
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/plan_enumerator.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    // static
    void QueryPlanner::getFields(MatchExpression* node, string prefix, unordered_set<string>* out) {
        // Leaf nodes with a path and some array operators.
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            out->insert(prefix + node->path().toString());
        }
        else if (Indexability::arrayUsesIndexOnChildren(node)) {
            // If the array uses an index on its children, it's something like
            // {foo : {$elemMatch: { bar: 1}}}, in which case the predicate is really over
            // foo.bar.
            //
            // When we have {foo: {$all: [{$elemMatch: {a:1}}], the path of the embedded elemMatch
            // is empty.  We don't want to append a dot in that case as the field would be foo..a.
            if (!node->path().empty()) {
                prefix += node->path().toString() + ".";
            }

            for (size_t i = 0; i < node->numChildren(); ++i) {
                getFields(node->getChild(i), prefix, out);
            }
        }
        else if (node->isLogical()) {
            for (size_t i = 0; i < node->numChildren(); ++i) {
                getFields(node->getChild(i), prefix, out);
            }
        }
    }

    // static
    void QueryPlanner::findRelevantIndices(const unordered_set<string>& fields,
                                           const vector<IndexEntry>& allIndices,
                                           vector<IndexEntry>* out) {
        for (size_t i = 0; i < allIndices.size(); ++i) {
            BSONObjIterator it(allIndices[i].keyPattern);
            verify(it.more());
            BSONElement elt = it.next();
            if (fields.end() != fields.find(elt.fieldName())) {
                out->push_back(allIndices[i]);
            }
        }
    }

    // static
    bool QueryPlanner::compatible(const BSONElement& elt, const IndexEntry& index, MatchExpression* node) {
        // XXX: CatalogHack::getAccessMethodName: do we have to worry about this?  when?
        string ixtype;
        if (String != elt.type()) {
            ixtype = "";
        }
        else {
            ixtype = elt.String();
        }

        // we know elt.fieldname() == node->path()

        MatchExpression::MatchType exprtype = node->matchType();

        // TODO: use indexnames
        if ("" == ixtype) {
            // TODO: MatchExpression::TEXT, when it exists.
            return exprtype != MatchExpression::GEO && exprtype != MatchExpression::GEO_NEAR;
        }
        else if ("hashed" == ixtype) {
            // TODO: Any others?
            return exprtype == MatchExpression::MATCH_IN || exprtype == MatchExpression::EQ;
        }
        else if ("2dsphere" == ixtype) {
            // TODO Geo issues: Parsing is very well encapsulated in GeoQuery for 2dsphere right now
            // but for 2d it's not really parsed in the tree.  Needs auditing.
            return false;
        }
        else if ("2d" == ixtype) {
            // TODO : Geo issues: see db/index_selection.cpp.  I don't think 2d is parsed.  Perhaps
            // we can parse it as a blob with the verification done ala index_selection.cpp?
            // Really, we should fully validate the syntax.
            return false;
        }
        else if ("text" == ixtype || "_fts" == ixtype || "geoHaystack" == ixtype) {
            return false;
        }
        else {
            warning() << "Unknown indexing for for node " << node->toString()
                      << " and field " << elt.toString() << endl;
            verify(0);
        }
    }

    // static
    void QueryPlanner::rateIndices(MatchExpression* node, string prefix,
                                   const vector<IndexEntry>& indices) {
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            string fullPath = prefix + node->path().toString();
            verify(NULL == node->getTag());
            RelevantTag* rt = new RelevantTag();
            node->setTag(rt);

            // TODO: This is slow, with all the string compares.
            for (size_t i = 0; i < indices.size(); ++i) {
                BSONObjIterator it(indices[i].keyPattern);
                BSONElement elt = it.next();
                if (elt.fieldName() == fullPath && compatible(elt, indices[i], node)) {
                    rt->first.push_back(i);
                }
                while (it.more()) {
                    elt = it.next();
                    if (elt.fieldName() == fullPath && compatible(elt, indices[i], node)) {
                        rt->notFirst.push_back(i);
                    }
                }
            }
        }
        else if (Indexability::arrayUsesIndexOnChildren(node)) {
            // See comment in getFields about all/elemMatch and paths.
            if (!node->path().empty()) {
                prefix += node->path().toString() + ".";
            }
            for (size_t i = 0; i < node->numChildren(); ++i) {
                rateIndices(node->getChild(i), prefix, indices);
            }
        }
        else if (node->isLogical()) {
            for (size_t i = 0; i < node->numChildren(); ++i) {
                rateIndices(node->getChild(i), prefix, indices);
            }
        }
    }

    // static
    QuerySolution* QueryPlanner::makeCollectionScan(const CanonicalQuery& query, bool tailable) {
        // Make the (only) node, a collection scan.
        CollectionScanNode* csn = new CollectionScanNode();
        csn->name = query.ns();
        csn->filter.reset(query.root()->shallowClone());
        csn->tailable = tailable;

        // If the sort is {$natural: +-1} this changes the direction of the collection scan.
        const BSONObj& sortObj = query.getParsed().getSort();
        if (!sortObj.isEmpty()) {
            BSONElement natural = sortObj.getFieldDotted("$natural");
            if (!natural.eoo()) {
                csn->direction = natural.numberInt() >= 0 ? 1 : -1;
            }
        }

        // The hint can specify $natural as well.
        if (!query.getParsed().getHint().isEmpty()) {
            BSONElement natural = query.getParsed().getHint().getFieldDotted("$natural");
            if (!natural.eoo()) {
                csn->direction = natural.numberInt() >= 0 ? 1 : -1;
            }
        }

        // cout << "Outputting collscan " << soln->toString() << endl;
        return analyzeDataAccess(query, csn);
    }

    // static
    IndexScanNode* QueryPlanner::makeIndexScan(const BSONObj& indexKeyPattern,
                                               MatchExpression* expr,
                                               bool* exact) {
        cout << "making ixscan for " << expr->toString() << endl;

        // Note that indexKeyPattern.firstElement().fieldName() may not equal expr->path() because
        // expr might be inside an array operator that provides a path prefix.
        IndexScanNode* isn = new IndexScanNode();
        isn->indexKeyPattern = indexKeyPattern;
        isn->bounds.fields.resize(indexKeyPattern.nFields());
        if (MatchExpression::ELEM_MATCH_VALUE == expr->matchType()) {
            // Root is tagged with an index.  We have predicates over root's path.  Pick one
            // to define the bounds.

            // TODO: We could/should merge the bounds (possibly subject to various multikey
            // etc.  restrictions).  For now we don't bother.
            IndexBoundsBuilder::translate(expr->getChild(0), indexKeyPattern.firstElement(),
                                          &isn->bounds.fields[0], exact);
            // TODO: I think this is valid but double check.
            *exact = false;
        }
        else {
            IndexBoundsBuilder::translate(expr, indexKeyPattern.firstElement(),
                                          &isn->bounds.fields[0], exact);
        }

        cout << "bounds are " << isn->bounds.toString() << " exact " << *exact << endl;

        return isn;
    }

    // static
    void QueryPlanner::finishIndexScan(IndexScanNode* scan, const BSONObj& indexKeyPattern) {
        // Find the first field in the scan's bounds that was not filled out.
        size_t firstEmptyField = 0;
        for (firstEmptyField = 0; firstEmptyField < scan->bounds.fields.size(); ++firstEmptyField) {
            if ("" == scan->bounds.fields[firstEmptyField].name) {
                verify(scan->bounds.fields[firstEmptyField].intervals.empty());
                break;
            }
        }

        // All fields are filled out with bounds, nothing to do.
        if (firstEmptyField == scan->bounds.fields.size()) { return; }

        // Skip ahead to the firstEmptyField-th element, where we begin filling in bounds.
        BSONObjIterator it(indexKeyPattern);
        for (size_t i = 0; i < firstEmptyField; ++i) {
            verify(it.more());
            it.next();
        }

        // For each field in the key...
        while (it.more()) {
            // Be extra sure there's no data there.
            verify("" == scan->bounds.fields[firstEmptyField].name);
            verify(scan->bounds.fields[firstEmptyField].intervals.empty());
            // ...build the "all values" interval.
            IndexBoundsBuilder::allValuesForField(it.next(), &scan->bounds.fields[firstEmptyField]);
            ++firstEmptyField;
        }

        // Make sure that the length of the key is the length of the bounds we started.
        verify(firstEmptyField == scan->bounds.fields.size());
    }

    // static
    bool QueryPlanner::processIndexScans(MatchExpression* root,
                                         bool inArrayOperator,
                                         const vector<IndexEntry>& indices,
                                         vector<QuerySolutionNode*>* out) {

        bool isAnd = MatchExpression::AND == root->matchType();
        if (!isAnd) {
            verify(MatchExpression::OR == root->matchType());
        }

        auto_ptr<IndexScanNode> currentScan;
        size_t currentIndexNumber = IndexTag::kNoIndex;
        size_t nextIndexPos = 0;
        size_t curChild = 0;

        // This 'while' processes all IXSCANs, possibly merging scans by combining the bounds.  We
        // can merge scans in two cases:
        // 1. Filling out subsequent fields in a compound index.
        // 2. Intersecting bounds.  Currently unimplemented.
        while (curChild < root->numChildren()) {
            MatchExpression* child = root->getChild(curChild);

            // If there is no tag, it's not using an index.  We've sorted our children such that the
            // children with tags are first, so we stop now.
            if (NULL == child->getTag()) { break; }

            IndexTag* ixtag = static_cast<IndexTag*>(child->getTag());
            // If there's a tag it must be valid.
            verify(IndexTag::kNoIndex != ixtag->index);

            // If the child can't use an index on its own field, it's indexed by virtue of one of
            // its children having an index.  We don't do anything special here, just add it to
            // the output as-is.
            //
            // NOTE: If the child is logical, it could possibly collapse into a single ixscan.  we
            // ignore this for now.
            if (!Indexability::nodeCanUseIndexOnOwnField(child)) {
                if (!inArrayOperator) {
                    // The logical sub-tree is responsible for fully evaluating itself.  Any
                    // required filters or fetches are already hung on it.  As such, we remove the
                    // filter branch from our tree.  buildIndexedDataAccess takes ownership of the
                    // child.
                    root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                    // The curChild of today is the curChild+1 of yesterday.
                }
                else {
                    ++curChild;
                }

                // If inArrayOperator: takes ownership of child, which is OK, since we detached
                // child from root.
                QuerySolutionNode* childSolution = buildIndexedDataAccess(child,
                                                                          inArrayOperator,
                                                                          indices);
                if (NULL == childSolution) { return false; }
                out->push_back(childSolution);
                continue;
            }

            // If we're here, we now know that 'child' can use an index directly and the index is
            // over the child's field.

            // If the child we're looking at uses a different index than the current index scan, add
            // the current index scan to the output as we're done with it.  The index scan created
            // by the child then becomes our new current index scan.  Note that the current scan
            // could be NULL, in which case we don't output it.  The rest of the logic is identical.
            //
            // If the child uses the same index as the current index scan, we may be able to merge
            // the bounds for the two scans.  See the comments below for canMergeBounds and
            // shouldMergeBounds.

            // Is it remotely possible to merge the bounds?  We care about isAnd because bounds
            // merging is currently totally unimplemented for ORs.
            //
            // TODO: When more than one child of an AND clause can have an index, we'll have to fix
            // the logic below to only merge when the merge is really filling out an additional
            // field of a compound index.
            //
            // TODO: Implement union of OILs to allow merging bounds for OR.
            // TODO: Implement intersection of OILs to allow merging of bounds for AND.

            bool canMergeBounds = (NULL != currentScan.get())
                                  && (currentIndexNumber == ixtag->index)
                                  && isAnd;

            // Is it semantically correct to merge bounds?
            //
            // Guiding principle: must the values we're testing come from the same array in the
            // document?  If so, we can combine bounds (via intersection or compounding).  If not,
            // we can't.
            //
            // If the index is NOT multikey, it's always semantically correct to combine bounds,
            // as there are no arrays to worry about.
            //
            // If the index is multikey, there are arrays of values.  There are three issues:
            //
            // 1. We can't intersect bounds even if the bounds are not on a compound index.
            //    Example:
            //    Let's say we have the document {a: [5, 7]}.
            //    This document satisfies the query {$and: [ {a: 5}, {a: 7} ] }
            //    For the index {a:1} we have the keys {"": 5} and {"": 7}.
            //    Each child of the AND is tagged with the index {a: 1}
            //    The interval for the {a: 5} branch is [5, 5].  It is exact.
            //    The interval for the {a: 7} branch is [7, 7].  It is exact.
            //    The intersection of the intervals is {}.
            //    If we scan over {}, the intersection of the intervals, we will retrieve nothing.
            //
            // 2. If we're using a compound index, we can only specify bounds for the first field.
            //    Example:
            //    Let's say we have the document {a: [ {b: 3}, {c: 4} ] }
            //    This document satisfies the query {'a.b': 3, 'a.c': 4}.
            //    For the index {'a.b': 1, 'a.c': 1} we have the keys {"": 3, "": null} and
            //                                                        {"": null, "": 4}.
            //    Let's use the aforementioned index to answer the query.
            //    The bounds for 'a.b' are [3,3], and the bounds for 'a.c' are [4,4].
            //    If we combine the bounds, we would only look at keys {"": 3, "":4 }.
            //    Therefore we wouldn't look at the document's keys in the index.
            //    Therefore we don't combine bounds.
            //
            // 3. There is an exception to (2), and that is when we're evaluating an $elemMatch.
            //    Example:
            //    Our query is a: {$elemMatch: {b:3, c:4}}.
            //    Let's say that we have the index {'a.b': 1, 'a.c': 1} as in (2).
            //    $elemMatch requires if a.b==3 and a.c==4, the predicates must be satisfied from
            //    the same array entry.
            //    If those values are both present in the same array, the index key for the
            //    aforementioned index will be {"":3, "":4}
            //    Therefore we can intersect bounds.
            //
            //    XXX: There is more to it than this.  See index13.js.

            bool shouldMergeBounds = (currentIndexNumber != IndexTag::kNoIndex)
                                     && !indices[currentIndexNumber].multikey;

            // XXX: commented out until cases in index13.js are resolved.
            // bool shouldMergeBounds = !indices[currentIndexNumber].multikey || inArrayOperator;

            if (!canMergeBounds || !shouldMergeBounds) {
                if (NULL != currentScan.get()) {
                    finishIndexScan(currentScan.get(), indices[currentIndexNumber].keyPattern);
                    out->push_back(currentScan.release());
                }
                else {
                    verify(IndexTag::kNoIndex == currentIndexNumber);
                }

                currentIndexNumber = ixtag->index;
                nextIndexPos = 1;

                bool exact = false;
                currentScan.reset(makeIndexScan(indices[currentIndexNumber].keyPattern,
                                                child, &exact));

                if (exact && !inArrayOperator) {
                    // The bounds answer the predicate, and we can remove the expression from the
                    // root.  NOTE(opt): Erasing entry 0, 1, 2, ... could be kind of n^2, maybe
                    // optimize later.
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);
                    delete child;
                    // Don't increment curChild.
                }
                else {
                    // We keep curChild in the node for affixing later as a filter.
                    ++curChild;
                }
            }
            else {
                // The child uses the same index we're currently building a scan for.  Merge
                // the bounds and filters.
                verify(currentIndexNumber == ixtag->index);
                verify(ixtag->pos == nextIndexPos);

                // Get the ixtag->pos-th element of indexKeyPatterns[currentIndexNumber].
                // TODO: cache this instead/with ixtag->pos?
                BSONObjIterator it(indices[currentIndexNumber].keyPattern);
                BSONElement keyElt = it.next();
                for (size_t i = 0; i < ixtag->pos; ++i) {
                    verify(it.more());
                    keyElt = it.next();
                }
                verify(!keyElt.eoo());
                bool exact = false;

                //cout << "current bounds are " << currentScan->bounds.toString() << endl;
                //cout << "node merging in " << child->toString() << endl;
                //cout << "merging with field " << keyElt.toString(true, true) << endl;
                //cout << "taking advantage of compound index "
                     //<< indices[currentIndexNumber].keyPattern.toString() << endl;

                // We know at this point that the only case where we do this is compound indices so
                // just short-cut and dump the bounds there.
                verify(currentScan->bounds.fields[ixtag->pos].name.empty());
                IndexBoundsBuilder::translate(child, keyElt, &currentScan->bounds.fields[ixtag->pos], &exact);

                if (exact) {
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);
                    delete child;
                }
                else {
                    // We keep curChild in the AND for affixing later.
                    ++curChild;
                }

                ++nextIndexPos;
            }
        }

        // Output the scan we're done with, if it exists.
        if (NULL != currentScan.get()) {
            finishIndexScan(currentScan.get(), indices[currentIndexNumber].keyPattern);
            out->push_back(currentScan.release());
        }

        return true;
    }

    // static
    QuerySolutionNode* QueryPlanner::buildIndexedAnd(MatchExpression* root,
                                                     bool inArrayOperator,
                                                     const vector<IndexEntry>& indices) {
        auto_ptr<MatchExpression> autoRoot;
        if (!inArrayOperator) {
            autoRoot.reset(root);
        }

        vector<QuerySolutionNode*> ixscanNodes;
        if (!processIndexScans(root, inArrayOperator, indices, &ixscanNodes)) {
            return NULL;
        }

        //
        // Process all non-indexed predicates.  We hang these above the AND with a fetch and
        // filter.
        //

        // This is the node we're about to return.
        QuerySolutionNode* andResult;

        // We must use an index for at least one child of the AND.  We shouldn't be here if this
        // isn't the case.
        verify(ixscanNodes.size() >= 1);

        // Short-circuit: an AND of one child is just the child.
        if (ixscanNodes.size() == 1) {
            andResult = ixscanNodes[0];
        }
        else {
            // Figure out if we want AndHashNode or AndSortedNode.
            bool allSortedByDiskLoc = true;
            for (size_t i = 0; i < ixscanNodes.size(); ++i) {
                if (!ixscanNodes[i]->sortedByDiskLoc()) {
                    allSortedByDiskLoc = false;
                    break;
                }
            }
            if (allSortedByDiskLoc) {
                AndSortedNode* asn = new AndSortedNode();
                asn->children.swap(ixscanNodes);
                andResult = asn;
            }
            else {
                AndHashNode* ahn = new AndHashNode();
                ahn->children.swap(ixscanNodes);
                andResult = ahn;
            }
        }

        // Don't bother doing any kind of fetch analysis lite if we're doing it anyway above us.
        if (inArrayOperator) {
            return andResult;
        }

        // If there are any nodes still attached to the AND, we can't answer them using the
        // index, so we put a fetch with filter.
        if (root->numChildren() > 0) {
            FetchNode* fetch = new FetchNode();
            verify(NULL != autoRoot.get());
            // Takes ownership.
            fetch->filter.reset(autoRoot.release());
            // takes ownership
            fetch->child.reset(andResult);
            andResult = fetch;
        }
        else {
            // root has no children, let autoRoot get rid of it when it goes out of scope.
        }

        return andResult;
    }

    // static
    QuerySolutionNode* QueryPlanner::buildIndexedOr(MatchExpression* root,
                                                    bool inArrayOperator,
                                                    const vector<IndexEntry>& indices) {
        auto_ptr<MatchExpression> autoRoot;
        if (!inArrayOperator) {
            autoRoot.reset(root);
        }

        size_t expectedNumberScans = root->numChildren();
        vector<QuerySolutionNode*> ixscanNodes;
        if (!processIndexScans(root, inArrayOperator, indices, &ixscanNodes)) {
            return NULL;
        }

        // Unlike an AND, an OR cannot have filters hanging off of it.  We stop processing
        // when any of our children lack index tags.  If a node lacks an index tag it cannot
        // be answered via an index.
        if (ixscanNodes.size() != expectedNumberScans || (!inArrayOperator && 0 != root->numChildren())) {
            warning() << "planner OR error, non-indexed child of OR.";
            // We won't enumerate an OR without indices for each child, so this isn't an issue, even
            // if we have an AND with an OR child -- we won't get here unless the OR is fully
            // indexed.
            return NULL;
        }

        QuerySolutionNode* orResult = NULL;

        // An OR of one node is just that node.
        if (1 == ixscanNodes.size()) {
            orResult = ixscanNodes[0];
        }
        else {
            // If each child is sorted by the same predicate, we can merge them and maintain
            // sorted order.
            bool haveSameSort;
            if (ixscanNodes[0]->getSort().isEmpty()) {
                haveSameSort = false;
            }
            else {
                haveSameSort = true;
                for (size_t i = 1; i < ixscanNodes.size(); ++i) {
                    if (0 != ixscanNodes[0]->getSort().woCompare(ixscanNodes[i]->getSort())) {
                        haveSameSort = false;
                        break;
                    }
                }
            }

            if (haveSameSort) {
                MergeSortNode* msn = new MergeSortNode();
                msn->sort = ixscanNodes[0]->getSort();
                msn->children.swap(ixscanNodes);
                orResult = msn;
            }
            else {
                OrNode* orn = new OrNode();
                orn->children.swap(ixscanNodes);
                orResult = orn;
            }
        }

        // OR must have an index for each child, so we should have detached all children from
        // 'root', and there's nothing useful to do with an empty or MatchExpression.  We let it die
        // via autoRoot.

        return orResult;
    }

    // static
    QuerySolutionNode* QueryPlanner::buildIndexedDataAccess(MatchExpression* root,
                                                            bool inArrayOperator,
                                                            const vector<IndexEntry>& indices) {
        if (root->isLogical()) {
            if (MatchExpression::AND == root->matchType()) {
                // Takes ownership of root.
                return buildIndexedAnd(root, inArrayOperator, indices);
            }
            else if (MatchExpression::OR == root->matchType()) {
                // Takes ownership of root.
                return buildIndexedOr(root, inArrayOperator, indices);
            }
            else {
                // Can't do anything with negated logical nodes index-wise.
                return NULL;
            }
        }
        else {
            auto_ptr<MatchExpression> autoRoot;
            if (!inArrayOperator) {
                autoRoot.reset(root);
            }

            // isArray or isLeaf is true.  Either way, it's over one field, and the bounds builder
            // deals with it.
            if (NULL == root->getTag()) {
                // No index to use here, not in the context of logical operator, so we're SOL.
                return NULL;
            }
            else if (Indexability::nodeCanUseIndexOnOwnField(root)) {
                // Make an index scan over the tagged index #.
                IndexTag* tag = static_cast<IndexTag*>(root->getTag());

                bool exact = false;
                auto_ptr<IndexScanNode> isn;
                isn.reset(makeIndexScan(indices[tag->index].keyPattern, root, &exact));

                finishIndexScan(isn.get(), indices[tag->index].keyPattern);

                if (inArrayOperator) {
                    return isn.release();
                }

                // If the bounds are exact, the set of documents that satisfy the predicate is
                // exactly equal to the set of documents that the scan provides.
                //
                // If the bounds are not exact, the set of documents returned from the scan is a
                // superset of documents that satisfy the predicate, and we must check the
                // predicate.
                if (!exact) {
                    FetchNode* fetch = new FetchNode();
                    verify(NULL != autoRoot.get());
                    fetch->filter.reset(autoRoot.release());
                    fetch->child.reset(isn.release());
                    return fetch;
                }

                return isn.release();
            }
            else if (Indexability::arrayUsesIndexOnChildren(root)) {
                QuerySolutionNode* solution = NULL;

                if (MatchExpression::ALL == root->matchType()) {
                    // Here, we formulate an AND of all the sub-clauses.
                    auto_ptr<AndHashNode> ahn(new AndHashNode());

                    for (size_t i = 0; i < root->numChildren(); ++i) {
                        QuerySolutionNode* node = buildIndexedDataAccess(root->getChild(i), true,
                                                                         indices);
                        if (NULL != node) {
                            ahn->children.push_back(node);
                        }
                    }

                    // No children, no point in hashing nothing.
                    if (0 == ahn->children.size()) { return NULL; }

                    // AND of one child is just that child.
                    if (1 == ahn->children.size()) {
                        solution = ahn->children[0];
                        ahn->children.clear();
                        ahn.reset();
                    }
                    else {
                        // More than one child.
                        solution = ahn.release();
                    }
                }
                else {
                    verify(MatchExpression::ELEM_MATCH_OBJECT);
                    // The child is an AND.
                    verify(1 == root->numChildren());
                    solution = buildIndexedDataAccess(root->getChild(0), true, indices);
                    if (NULL == solution) { return NULL; }
                }

                // There may be an array operator above us.
                if (inArrayOperator) { return solution; }

                FetchNode* fetch = new FetchNode();
                // Takes ownership of 'root'.
                verify(NULL != autoRoot.get());
                fetch->filter.reset(autoRoot.release());
                fetch->child.reset(solution);
                return fetch;
            }
        }

        return NULL;
    }

    // static
    QuerySolution* QueryPlanner::analyzeDataAccess(const CanonicalQuery& query,
                                                   QuerySolutionNode* solnRoot) {
        auto_ptr<QuerySolution> soln(new QuerySolution());
        soln->filterData = query.getQueryObj();
        verify(soln->filterData.isOwned());
        soln->ns = query.ns();

        // solnRoot finds all our results.  Let's see what transformations we must perform to the
        // data.

        // Sort the results, if there is a sort specified.
        if (!query.getParsed().getSort().isEmpty()) {
            const BSONObj& sortObj = query.getParsed().getSort();

            // If the sort is $natural, we ignore it, assuming that the caller has detected that and
            // outputted a collscan to satisfy the desired order.
            BSONElement natural = sortObj.getFieldDotted("$natural");
            if (natural.eoo()) {
                // See if solnRoot gives us the sort.  If so, we're done.
                if (0 == sortObj.woCompare(solnRoot->getSort())) {
                    // Sort is already provided!
                }
                else {
                    // If solnRoot isn't already sorted, let's see if it has the fields we're
                    // sorting on.  If it's fetched, it has all the fields by definition.  If it's
                    // not, we check sort field by sort field.
                    if (!solnRoot->fetched()) {
                        bool sortCovered = true;
                        BSONObjIterator it(sortObj);
                        while (it.more()) {
                            if (!solnRoot->hasField(it.next().fieldName())) {
                                sortCovered = false;
                                break;
                            }
                        }

                        if (!sortCovered) {
                            FetchNode* fetch = new FetchNode();
                            fetch->child.reset(solnRoot);
                            solnRoot = fetch;
                        }
                    }

                    soln->hasSortStage = true;
                    SortNode* sort = new SortNode();
                    sort->pattern = sortObj;
                    sort->child.reset(solnRoot);
                    solnRoot = sort;
                }
            }
        }

        // Project the results.
        if (NULL != query.getProj()) {
            if (query.getProj()->requiresDocument()) {
                // If the projection requires the entire document, somebody must fetch.
                if (!solnRoot->fetched()) {
                    FetchNode* fetch = new FetchNode();
                    fetch->child.reset(solnRoot);
                    solnRoot = fetch;
                }
            }
            else {
                const vector<string>& fields = query.getProj()->requiredFields();
                bool covered = true;
                for (size_t i = 0; i < fields.size(); ++i) {
                    if (!solnRoot->hasField(fields[i])) {
                        covered = false;
                        break;
                    }
                }
                // If any field is missing from the list of fields the projection wants,
                // a fetch is required.
                if (!covered) {
                    FetchNode* fetch = new FetchNode();
                    fetch->child.reset(solnRoot);
                    solnRoot = fetch;
                }
            }

            // We now know we have whatever data is required for the projection.
            ProjectionNode* projNode = new ProjectionNode();
            projNode->projection = query.getProj();
            projNode->child.reset(solnRoot);
            solnRoot = projNode;
        }
        else {
            // If there's no projection, we must fetch, as the user wants the entire doc.
            if (!solnRoot->fetched()) {
                FetchNode* fetch = new FetchNode();
                fetch->child.reset(solnRoot);
                solnRoot = fetch;
            }
        }

        if (0 != query.getParsed().getSkip()) {
            SkipNode* skip = new SkipNode();
            skip->skip = query.getParsed().getSkip();
            skip->child.reset(solnRoot);
            solnRoot = skip;
        }

        soln->root.reset(solnRoot);
        return soln.release();
    }

    /**
     * Does the tree rooted at 'root' have a node with matchType 'type'?
     */
    bool hasNode(MatchExpression* root, MatchExpression::MatchType type) {
        if (type == root->matchType()) {
            return true;
        }
        for (size_t i = 0; i < root->numChildren(); ++i) {
            if (hasNode(root->getChild(i), type)) {
                return true;
            }
        }
        return false;
    }

    // Copied verbatim from db/index.h
    static bool isIdIndex( const BSONObj &pattern ) {
        BSONObjIterator i(pattern);
        BSONElement e = i.next();
        //_id index must have form exactly {_id : 1} or {_id : -1}.
        //Allows an index of form {_id : "hashed"} to exist but
        //do not consider it to be the primary _id index
        if(! ( strcmp(e.fieldName(), "_id") == 0
                && (e.numberInt() == 1 || e.numberInt() == -1)))
            return false;
        return i.next().eoo();
    }

    // static
    void QueryPlanner::plan(const CanonicalQuery& query, const vector<IndexEntry>& indices,
                            size_t options, vector<QuerySolution*>* out) {
        cout << "Begin planning.\nquery = " << query.toString() << endl;

        bool canTableScan = !(options & QueryPlanner::NO_TABLE_SCAN);

        // XXX: If pq.hasOption(QueryOption_OplogReplay) use FindingStartCursor equivalent which
        // must be translated into stages.

        // If the query requests a tailable cursor, the only solution is a collscan + filter with
        // tailable set on the collscan.  TODO: This is a policy departure.  Previously I think you
        // could ask for a tailable cursor and it just tried to give you one.  Now, we fail if we
        // can't provide one.  Is this what we want?
        if (query.getParsed().hasOption(QueryOption_CursorTailable)) {
            if (!hasNode(query.root(), MatchExpression::GEO_NEAR) && canTableScan) {
                out->push_back(makeCollectionScan(query, true));
            }
            return;
        }

        // The hint can be $natural: 1.  If this happens, output a collscan.  It's a weird way of
        // saying "table scan for two, please."
        if (!query.getParsed().getHint().isEmpty()) {
            BSONElement natural = query.getParsed().getHint().getFieldDotted("$natural");
            if (!natural.eoo()) {
                cout << "forcing a table scan due to hinted $natural\n";
                if (canTableScan) {
                    out->push_back(makeCollectionScan(query, false));
                }
                return;
            }
        }

        // NOR and NOT we can't handle well with indices.  If we see them here, they weren't
        // rewritten to remove the negation.  Just output a collscan for those.
        if (hasNode(query.root(), MatchExpression::NOT)
            || hasNode(query.root(), MatchExpression::NOR)) {

            // If there's a near predicate, we can't handle this.
            // TODO: Should canonicalized query detect this?
            if (hasNode(query.root(), MatchExpression::GEO_NEAR)) {
                warning() << "Can't handle NOT/NOR with GEO_NEAR";
                return;
            }
            cout << "NOT/NOR in plan, just outtping a collscan\n";
            if (canTableScan) {
                out->push_back(makeCollectionScan(query, false));
            }
            return;
        }

        // Figure out what fields we care about.
        unordered_set<string> fields;
        getFields(query.root(), "", &fields);
        /*
        for (unordered_set<string>::const_iterator it = fields.begin(); it != fields.end(); ++it) {
            cout << "field " << *it << endl;
        }
        */

        // Filter our indices so we only look at indices that are over our predicates.
        vector<IndexEntry> relevantIndices;

        // Hints require us to only consider the hinted index.
        BSONObj hintIndex = query.getParsed().getHint();

        // Snapshot is a form of a hint.  If snapshot is set, try to use _id index to make a real
        // plan.  If that fails, just scan the _id index.
        if (query.getParsed().isSnapshot()) {
            // Find the ID index in indexKeyPatterns.  It's our hint.
            for (size_t i = 0; i < indices.size(); ++i) {
                if (isIdIndex(indices[i].keyPattern)) {
                    hintIndex = indices[i].keyPattern;
                    break;
                }
            }
        }

        if (!hintIndex.isEmpty()) {
            bool hintValid = false;
            for (size_t i = 0; i < indices.size(); ++i) {
                if (0 == indices[i].keyPattern.woCompare(hintIndex)) {
                    relevantIndices.clear();
                    relevantIndices.push_back(indices[i]);
                    cout << "hint specified, restricting indices to " << hintIndex.toString() << endl;
                    hintValid = true;
                    break;
                }
            }
            if (!hintValid) {
                warning() << "Hinted index " << hintIndex.toString() << " does not exist, ignoring.";
            }
        }
        else {
            findRelevantIndices(fields, indices, &relevantIndices);
        }

        // Figure out how useful each index is to each predicate.
        // query.root() is now annotated with RelevantTag(s).
        rateIndices(query.root(), "", relevantIndices);

        // If we have any relevant indices, we try to create indexed plans.
        if (0 < relevantIndices.size()) {
            /*
            for (size_t i = 0; i < relevantIndices.size(); ++i) {
                cout << "relevant idx " << i << " is " << relevantIndices[i].toString() << endl;
            }
            */

            // The enumerator spits out trees tagged with IndexTag(s).
            PlanEnumerator isp(query.root(), &relevantIndices);
            isp.init();

            MatchExpression* rawTree;
            while (isp.getNext(&rawTree)) {
                cout << "about to build solntree from tagged tree:\n" << rawTree->toString()
                     << endl;

                // This can fail if enumeration makes a mistake.
                QuerySolutionNode* solnRoot = buildIndexedDataAccess(rawTree, false, relevantIndices);
                if (NULL == solnRoot) { continue; }

                // This shouldn't ever fail.
                QuerySolution* soln = analyzeDataAccess(query, solnRoot);
                verify(NULL != soln);

                cout << "Adding solution:\n" << soln->toString() << endl;
                out->push_back(soln);
            }
        }

        // An index was hinted.  If there are any solutions, they use the hinted index.  If not, we
        // scan the entire index to provide results and output that as our plan.  This is the
        // desired behavior when an index is hinted that is not relevant to the query.
        if (!hintIndex.isEmpty() && (0 == out->size())) {
            // Build an ixscan over the id index, use it, and return it.
            IndexScanNode* isn = new IndexScanNode();
            isn->indexKeyPattern = hintIndex;
            isn->bounds.fields.resize(hintIndex.nFields());

            // TODO: can we use simple bounds with this compound idx?
            BSONObjIterator it(isn->indexKeyPattern);
            int field = 0;
            while (it.more()) {
                IndexBoundsBuilder::allValuesForField(it.next(), &isn->bounds.fields[field]);
                ++field;
            }

            // TODO: We may not need to do the fetch if the predicates in root are covered.  But
            // for now it's safe (though *maybe* slower).
            FetchNode* fetch = new FetchNode();
            fetch->filter.reset(query.root()->shallowClone());
            fetch->child.reset(isn);

            QuerySolution* soln = analyzeDataAccess(query, fetch);
            verify(NULL != soln);
            out->push_back(soln);

            cout << "using hinted index as scan, soln = " << soln->toString() << endl;
            return;
        }

        // If a sort order is requested, there may be an index that provides it, even if that
        // index is not over any predicates in the query.
        //
        // XXX XXX: Can we do this even if the index is sparse?  Might we miss things?
        if (!query.getParsed().getSort().isEmpty()) {
            // See if we have a sort provided from an index already.
            bool usingIndexToSort = false;
            for (size_t i = 0; i < out->size(); ++i) {
                QuerySolution* soln = (*out)[i];
                if (!soln->hasSortStage) {
                    usingIndexToSort = true;
                    break;
                }
            }

            if (!usingIndexToSort) {
                for (size_t i = 0; i < indices.size(); ++i) {
                    const BSONObj& kp = indices[i].keyPattern;
                    if (0 == kp.woCompare(query.getParsed().getSort())) {
                        IndexScanNode* isn = new IndexScanNode();
                        isn->indexKeyPattern = kp;
                        isn->bounds.fields.resize(kp.nFields());

                        // TODO: can we use simple bounds if compound?
                        BSONObjIterator it(isn->indexKeyPattern);
                        size_t field = 0;
                        while (it.more()) {
                            IndexBoundsBuilder::allValuesForField(it.next(),
                                                                  &isn->bounds.fields[field]);
                        }

                        // TODO: We may not need to do the fetch if the predicates in root are
                        // covered.  But for now it's safe (though *maybe* slower).
                        FetchNode* fetch = new FetchNode();
                        fetch->filter.reset(query.root()->shallowClone());
                        fetch->child.reset(isn);

                        QuerySolution* soln = analyzeDataAccess(query, fetch);
                        verify(NULL != soln);
                        out->push_back(soln);
                        cout << "using index to provide sort, soln = " << soln->toString() << endl;
                        break;
                    }
                }
            }
        }

        // TODO: Do we always want to offer a collscan solution?
        // XXX: currently disabling the always-use-a-collscan in order to find more planner bugs.
        if (!hasNode(query.root(), MatchExpression::GEO_NEAR)
            && ((options & QueryPlanner::INCLUDE_COLLSCAN) || (0 == out->size() && canTableScan))) {
            QuerySolution* collscan = makeCollectionScan(query, false);
            out->push_back(collscan);
            cout << "Outputting a collscan\n";
        }
    }

}  // namespace mongo
