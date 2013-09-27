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
#include "mongo/db/query/plan_enumerator.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    void getFields(MatchExpression* node, unordered_set<string>* out) {
        StringData path = node->path();
        if (!path.empty()) {
            out->insert(path.toString());
        }

        // XXX Do we do this if the node is logical, or if it's array, or both?
        for (size_t i = 0; i < node->numChildren(); ++i) {
            getFields(node->getChild(i), out);
        }
    }

    /**
     * Find all indices prefixed by fields we have predicates over.  Only these indices are useful
     * in answering the query.
     */
    void findRelevantIndices(const unordered_set<string>& fields, const vector<BSONObj>& allIndices,
                             vector<BSONObj>* out) {
        for (size_t i = 0; i < allIndices.size(); ++i) {
            BSONObjIterator it(allIndices[i]);
            verify(it.more());
            BSONElement elt = it.next();
            if (fields.end() != fields.find(elt.fieldName())) {
                out->push_back(allIndices[i]);
            }
        }
    }

    bool compatible(const BSONElement& elt, MatchExpression* node) {
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

        // XXX use IndexNames

        if ("" == ixtype) {
            // TODO: MatchExpression::TEXT, when it exists.
            return exprtype != MatchExpression::GEO && exprtype != MatchExpression::GEO_NEAR;
        }
        else if ("hashed" == ixtype) {
            // TODO: Any others?
            return exprtype == MatchExpression::MATCH_IN || exprtype == MatchExpression::EQ;
        }
        else if ("2dsphere" == ixtype) {
            // XXX Geo issues: Parsing is very well encapsulated in GeoQuery for 2dsphere right now
            // but for 2d it's not really parsed in the tree.  Needs auditing.
            return false;
        }
        else if ("2d" == ixtype) {
            // XXX: Geo issues: see db/index_selection.cpp.  I don't think 2d is parsed.  Perhaps we
            // can parse it as a blob with the verification done ala index_selection.cpp?  Really,
            // we should fully validate the syntax.
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

    void rateIndices(MatchExpression* node, const vector<BSONObj>& indices) {
        if (node->isArray() || node->isLeaf()) {
            string path = node->path().toString();
            if (!path.empty()) {
                verify(NULL == node->getTag());
                RelevantTag* rt = new RelevantTag();
                node->setTag(rt);

                // TODO: This is slow, with all the string compares.
                for (size_t i = 0; i < indices.size(); ++i) {
                    BSONObjIterator it(indices[i]);
                    BSONElement elt = it.next();
                    if (elt.fieldName() == path && compatible(elt, node)) {
                        rt->first.push_back(i);
                    }
                    while (it.more()) {
                        elt = it.next();
                        if (elt.fieldName() == path && compatible(elt, node)) {
                            rt->notFirst.push_back(i);
                        }
                    }
                }
            }
        }

        for (size_t i = 0; i < node->numChildren(); ++i) {
            rateIndices(node->getChild(i), indices);
        }
    }

    QuerySolution* makeCollectionScan(const CanonicalQuery& query, bool tailable) {
        auto_ptr<QuerySolution> soln(new QuerySolution());
        soln->filter.reset(query.root()->shallowClone());
        // BSONValue, where are you?
        soln->filterData = query.getQueryObj();
        verify(soln->filterData.isOwned());
        soln->ns = query.ns();

        // Make the (only) node, a collection scan.
        CollectionScanNode* csn = new CollectionScanNode();
        csn->name = query.ns();
        csn->filter = soln->filter.get();
        csn->tailable = tailable;

        QuerySolutionNode* solnRoot = csn;

        // XXX: call analyzeDataAccess to do the work below once $natural is dealt with better.

        // TODO: We need better checking for this.  Should be done in CanonicalQuery and we should
        // be able to assume it's correct.
        const BSONObj& sortObj = query.getParsed().getSort();

        if (!sortObj.isEmpty()) {
            BSONElement natural = sortObj.getFieldDotted("$natural");
            if (!natural.eoo()) {
                csn->direction = natural.numberInt() >= 0 ? 1 : -1;
            }
            else {
                soln->hasSortStage = true;
                SortNode* sort = new SortNode();
                sort->pattern = sortObj;
                sort->child.reset(csn);
                solnRoot = sort;
            }
        }

        // The hint can specify $natural as well.  Check there to see if the direction is provided.
        if (!query.getParsed().getHint().isEmpty()) {
            BSONElement natural = query.getParsed().getHint().getFieldDotted("$natural");
            if (!natural.eoo()) {
                csn->direction = natural.numberInt() >= 0 ? 1 : -1;
            }
        }

        if (NULL != query.getProj()) {
            ProjectionNode* proj = new ProjectionNode();
            proj->projection = query.getProj();
            proj->child.reset(solnRoot);
            solnRoot = proj;
        }

        if (0 != query.getParsed().getSkip()) {
            SkipNode* skip = new SkipNode();
            skip->skip = query.getParsed().getSkip();
            skip->child.reset(solnRoot);
            solnRoot = skip;
        }

        soln->root.reset(solnRoot);
        // cout << "Outputting collscan " << soln->toString() << endl;
        return soln.release();
    }

    IndexScanNode* makeIndexScan(const BSONObj& indexKeyPattern, MatchExpression* expr,
                                 bool* exact) {
        IndexScanNode* isn = new IndexScanNode();
        isn->indexKeyPattern = indexKeyPattern;

        if (indexKeyPattern.firstElement().fieldName() != expr->path().toString()) {
            cout << "trying to build index w/bad prefix, indexkp fn is "
                 << indexKeyPattern.firstElement().fieldName()
                 << " path is " << expr->path().toString() << endl;
            verify(0);
        }

        BSONObjIterator it(isn->indexKeyPattern);
        BSONElement elt = it.next();
        isn->bounds.fields.resize(indexKeyPattern.nFields());
        IndexBoundsBuilder::translate(expr, elt, &isn->bounds.fields[0], exact);
        return isn;
    }

    /**
     * Fill in any bounds that are missing in 'scan' with the "all values for this field" interval.
     */
    void finishIndexScan(IndexScanNode* scan, const BSONObj& indexKeyPattern) {
        cout << "bounds pre-finishing " << scan->bounds.toString() << endl;
        size_t firstEmptyField = 0;
        for (firstEmptyField = 0; firstEmptyField < scan->bounds.fields.size(); ++firstEmptyField) {
            if ("" == scan->bounds.fields[firstEmptyField].name) {
                break;
            }
        }
        if (firstEmptyField == scan->bounds.fields.size()) {
            cout << "bounds post-finishing " << scan->bounds.toString() << endl;
            return;
        }

        BSONObjIterator it(indexKeyPattern);
        for (size_t i = 0; i < firstEmptyField; ++i) {
            it.next();
        }
        while (it.more()) {
            verify("" == scan->bounds.fields[firstEmptyField].name);
            // TODO: this is a surplus copy
            scan->bounds.fields[firstEmptyField++] = IndexBoundsBuilder::allValuesForField(it.next());
        }
        cout << "bounds post-finishing " << scan->bounds.toString() << endl;
    }

    QuerySolutionNode* buildIndexedDataAccess(MatchExpression* root,
                                              const vector<BSONObj>& indexKeyPatterns) {
        if (root->isLogical()) {
            // The children of AND and OR nodes are sorted by the index that the subtree rooted at
            // that node uses.  Child nodes that use the same index are adjacent to one another to
            // facilitate grouping of index scans.
            //
            // See tagForSort and sortUsingTags in index_tag.h
            if (MatchExpression::AND == root->matchType()) {
                // XXX: If all children are sortedByDiskLoc() this should be AndSortedNode.
                auto_ptr<AndHashNode> theAnd(new AndHashNode());

                // Process all IXSCANs, possibly combining them.
                auto_ptr<IndexScanNode> currentScan;
                size_t currentIndexNumber = IndexTag::kNoIndex;
                size_t curChild = 0;
                while (curChild < root->numChildren()) {
                    MatchExpression* child = root->getChild(curChild);

                    // No tag, it's not an IXSCAN.  We've sorted our children such that the tagged
                    // children are first, so we stop now.
                    if (NULL == child->getTag()) { break; }

                    IndexTag* ixtag = static_cast<IndexTag*>(child->getTag());
                    // If there's a tag it must be valid.
                    verify(IndexTag::kNoIndex != ixtag->index);

                    // TODO(opt): If the child is logical, it could collapse into an ixscan.  We
                    // ignore this for now.
                    if (child->isLogical()) {
                        QuerySolutionNode* childSolution = buildIndexedDataAccess(child,
                                                                                  indexKeyPatterns);
                        if (NULL == childSolution) { return NULL; }
                        theAnd->children.push_back(childSolution);
                        // The logical sub-tree is responsible for fully evaluating itself.  Any
                        // required filters or fetches are already hung on it.  As such, we remove
                        // the filter branch from our tree.
                        // XXX: Verify this is the right policy.
                        root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                        // XXX: Do we delete the curChild-th child??
                        continue;
                    }

                    // We now know that 'child' can use an index and that it's a predicate over a
                    // field (leaf).  There is no current index scan, make the current scan this
                    // child.
                    if (NULL == currentScan.get()) {
                        verify(IndexTag::kNoIndex == currentIndexNumber);
                        currentIndexNumber = ixtag->index;

                        bool exact = false;
                        currentScan.reset(makeIndexScan(indexKeyPatterns[currentIndexNumber], child, &exact));

                        if (exact) {
                            // The bounds answer the predicate, and we can remove the expression from the root.
                            // TODO(opt): Erasing entry 0, 1, 2, ... could be kind of n^2, maybe optimize later.
                            root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                            // Don't increment curChild.
                            // XXX: Do we delete the curChild-th child??
                        }
                        else {
                            // We keep curChild in the AND for affixing later.
                            ++curChild;
                        }
                        continue;
                    }

                    // If the child uses a different index than the current index scan, and there is
                    // a valid current index scan, add the current index scan as a child to the AND,
                    // as we're done with it.
                    //
                    // The child then becomes our new current index scan.

                    bool childUsesNewIndex = (currentIndexNumber != ixtag->index);

                    if (childUsesNewIndex) {
                        verify(NULL != currentScan.get());
                        finishIndexScan(currentScan.get(), indexKeyPatterns[currentIndexNumber]);
                        theAnd->children.push_back(currentScan.release());
                        currentIndexNumber = ixtag->index;

                        bool exact = false;
                        currentScan.reset(makeIndexScan(indexKeyPatterns[currentIndexNumber], child, &exact));

                        if (exact) {
                            // The bounds answer the predicate.
                            // TODO(opt): Erasing entry 0, 1, 2, ... could be kind of n^2, maybe optimize later.
                            root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                            // XXX: Do we delete the curChild-th child??
                        }
                        else {
                            // We keep curChild in the AND for affixing later.
                            ++curChild;
                        }
                        continue;
                    }
                    else {
                        // The child uses the same index we're currently building a scan for.  Merge the
                        // bounds and filters.
                        verify(currentIndexNumber == ixtag->index);

                        // First, make the bounds.
                        OrderedIntervalList oil(child->path().toString());

                        // TODO(opt): We can cache this as part of the index rating process
                        BSONObjIterator kpIt(indexKeyPatterns[currentIndexNumber]);
                        BSONElement elt = kpIt.next();
                        while (elt.fieldName() != oil.name) {
                            verify(kpIt.more());
                            elt = kpIt.next();
                        }
                        verify(!elt.eoo());
                        bool exact = false;
                        IndexBoundsBuilder::translate(child, elt, &oil, &exact);

                        //cout << "current bounds are " << currentScan->bounds.toString() << endl;
                        //cout << "node merging in " << child->toString() << endl;
                        //cout << "taking advantage of compound index " << indexKeyPatterns[currentIndexNumber].toString() << endl;

                        // Merge the bounds with the existing.
                        currentScan->bounds.joinAnd(oil, indexKeyPatterns[currentIndexNumber]);

                        if (exact) {
                            // The bounds answer the predicate.
                            // TODO(opt): Erasing entry 0, 1, 2, ... could be kind of n^2, maybe optimize later.
                            root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                            // XXX: Do we delete the curChild-th child??
                        }
                        else {
                            // We keep curChild in the AND for affixing later.
                            ++curChild;
                        }
                    }
                }

                // Output the scan we're done with.
                if (NULL != currentScan.get()) {
                    finishIndexScan(currentScan.get(), indexKeyPatterns[currentIndexNumber]);
                    theAnd->children.push_back(currentScan.release());
                }

                //
                // Process all non-indexed predicates.  We hang these above the AND with a fetch and
                // filter.
                //

                // This is the node we're about to return.
                QuerySolutionNode* andResult;

                // We must use an index for at least one child of the AND.
                verify(theAnd->children.size() >= 1);

                // Short-circuit: an AND of one child is just the child.
                if (theAnd->children.size() == 1) {
                    andResult = theAnd->children[0];
                    theAnd->children.clear();
                    // Deletes theAnd but we cleared the children.
                    theAnd.reset();
                }
                else {
                    andResult = theAnd.release();
                }

                // If there are any nodes still attached to the AND, we can't answer them using the
                // index, so we put a fetch with filter.
                if (root->numChildren() > 0) {
                    FetchNode* fetch = new FetchNode();
                    fetch->filter = root;
                    // takes ownership
                    fetch->child.reset(andResult);
                    andResult = fetch;
                }
                else {
                    // XXX: If root has no children, who deletes it/owns it?  What happens?
                }

                return andResult;
            }
            else if (MatchExpression::OR == root->matchType()) {
                // XXX: If all children have the same getSort() this should be MergeSortNode
                auto_ptr<OrNode> theOr(new OrNode());

                // Process all IXSCANs, possibly combining them.
                auto_ptr<IndexScanNode> currentScan;
                size_t currentIndexNumber = IndexTag::kNoIndex;
                size_t curChild = 0;
                while (curChild < root->numChildren()) {
                    MatchExpression* child = root->getChild(curChild);

                    // No tag, it's not an IXSCAN.
                    if (NULL == child->getTag()) { break; }

                    IndexTag* ixtag = static_cast<IndexTag*>(child->getTag());
                    // If there's a tag it must be valid.
                    verify(IndexTag::kNoIndex != ixtag->index);

                    // TODO(opt): If the child is logical, it could collapse into an ixscan.  We
                    // ignore this for now.
                    if (child->isLogical()) {
                        QuerySolutionNode* childSolution = buildIndexedDataAccess(child,
                                                                                  indexKeyPatterns);
                        if (NULL == childSolution) { return NULL; }
                        theOr->children.push_back(childSolution);
                        // The logical sub-tree is responsible for fully evaluating itself.  Any
                        // required filters or fetches are already hung on it.  As such, we remove
                        // the filter branch from our tree.
                        // XXX: Verify this is the right policy.
                        // XXX: Do we delete the curChild-th child??
                        root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                        continue;
                    }

                    // We now know that 'child' can use an index and that it's a predicate over a
                    // field.  There is no current index scan, make the current scan this child.
                    if (NULL == currentScan.get()) {
                        verify(IndexTag::kNoIndex == currentIndexNumber);
                        currentIndexNumber = ixtag->index;

                        bool exact = false;
                        currentScan.reset(makeIndexScan(indexKeyPatterns[currentIndexNumber], child, &exact));

                        if (exact) {
                            // The bounds answer the predicate, and we can remove the expression from the root.
                            // TODO(opt): Erasing entry 0, 1, 2, ... could be kind of n^2, maybe optimize later.
                            root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                            // Don't increment curChild.
                            // XXX: Do we delete the curChild-th child??
                        }
                        else {
                            // We keep curChild in the OR for affixing later as a filter.
                            ++curChild;
                        }
                        continue;
                    }

                    // If the child uses a different index than the current index scan, and there is
                    // a valid current index scan, add the current index scan as a child to the AND,
                    // as we're done with it.
                    //
                    // The child then becomes our new current index scan.

                    // XXX temporary until we can merge bounds.
                    bool childUsesNewIndex = true;

                    // XXX: uncomment when we can combine ixscans via bounds merging.
                    // bool childUsesNewIndex = (currentIndexNumber != ixtag->index);

                    if (childUsesNewIndex) {
                        verify(NULL != currentScan.get());
                        finishIndexScan(currentScan.get(), indexKeyPatterns[currentIndexNumber]);
                        theOr->children.push_back(currentScan.release());
                        currentIndexNumber = ixtag->index;

                        bool exact = false;
                        currentScan.reset(makeIndexScan(indexKeyPatterns[currentIndexNumber], child, &exact));

                        if (exact) {
                            // The bounds answer the predicate.
                            // TODO(opt): Erasing entry 0, 1, 2, ... could be kind of n^2, maybe optimize later.
                            root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                            // XXX: Do we delete the curChild-th child??
                        }
                        else {
                            // We keep curChild in the AND for affixing later.
                            ++curChild;
                        }
                        continue;
                    }
                    else {
                        // The child uses the same index we're currently building a scan for.  Merge the
                        // bounds and filters.
                        verify(currentIndexNumber == ixtag->index);

                        // First, make the bounds.
                        OrderedIntervalList oil(child->path().toString());

                        // TODO(opt): We can cache this as part of the index rating process
                        BSONObjIterator kpIt(indexKeyPatterns[currentIndexNumber]);
                        BSONElement elt = kpIt.next();
                        while (elt.fieldName() != oil.name) {
                            verify(kpIt.more());
                            elt = kpIt.next();
                        }
                        verify(!elt.eoo());
                        bool exact = false;
                        IndexBoundsBuilder::translate(child, elt, &oil, &exact);

                        // Merge the bounds with the existing.
                        currentScan->bounds.joinOr(oil, indexKeyPatterns[currentIndexNumber]);

                        if (exact) {
                            // The bounds answer the predicate.
                            // TODO(opt): Erasing entry 0, 1, 2, ... could be kind of n^2, maybe optimize later.
                            root->getChildVector()->erase(root->getChildVector()->begin() + curChild);

                            // XXX: Do we delete the curChild-th child??
                        }
                        else {
                            // We keep curChild in the OR for affixing later.
                            ++curChild;
                        }
                    }
                }

                // Output the scan we're done with.
                if (NULL != currentScan.get()) {
                    finishIndexScan(currentScan.get(), indexKeyPatterns[currentIndexNumber]);
                    theOr->children.push_back(currentScan.release());
                }

                // Unlike an AND, an OR cannot have filters hanging off of it.  We stop processing
                // when any of our children lack index tags.  If a node lacks an index tag it cannot
                // be answered via an index.
                if (curChild != root->numChildren()) {
                    warning() << "planner OR error, non-indexed child of OR.";
                    return NULL;
                }

                // If there are any nodes still attached to the OR, we can't answer them using the
                // index, so we put a fetch with filter.
                if (root->numChildren() > 0) {
                    FetchNode* fetch = new FetchNode();
                    fetch->filter = root;
                    // takes ownership
                    fetch->child.reset(theOr.release());
                    return fetch;
                }
                else {
                    return theOr.release();
                }
            }
            else {
                // NOT or NOR, can't do anything.
                return NULL;
            }
        }
        else {
            // isArray or isLeaf is true.  Either way, it's over one field, and the bounds builder
            // deals with it.
            if (NULL == root->getTag()) {
                // No index to use here, not in the context of logical operator, so we're SOL.
                return NULL;
            }
            else {
                // Make an index scan over the tagged index #.
                IndexTag* tag = static_cast<IndexTag*>(root->getTag());

                bool exact = false;
                auto_ptr<IndexScanNode> isn(makeIndexScan(indexKeyPatterns[tag->index], root, &exact));
                finishIndexScan(isn.get(), indexKeyPatterns[tag->index]);

                // If the bounds are exact, the set of documents that satisfy the predicate is exactly
                // equal to the set of documents that the scan provides.
                //
                // If the bounds are not exact, the set of documents returned from the scan is a
                // superset of documents that satisfy the predicate, and we must check the
                // predicate.
                if (!exact) {
                    FetchNode* fetch = new FetchNode();
                    fetch->filter = root;
                    fetch->child.reset(isn.release());
                    return fetch;
                }

                return isn.release();
            }
        }
        return NULL;
    }

    QuerySolution* analyzeDataAccess(const CanonicalQuery& query, MatchExpression* taggedRoot,
                                     QuerySolutionNode* solnRoot) {
        auto_ptr<QuerySolution> soln(new QuerySolution());
        soln->filter.reset(taggedRoot);
        soln->filterData = query.getQueryObj();
        verify(soln->filterData.isOwned());
        soln->ns = query.ns();

        // solnRoot finds all our results.

        // Sort the results.
        if (!query.getParsed().getSort().isEmpty()) {
            // See if solnRoot gives us the sort.  If so, we're done.
            if (0 == query.getParsed().getSort().woCompare(solnRoot->getSort())) {
                // Sort is already provided!
            }
            else {
                // If solnRoot isn't already sorted, let's see if it has the fields we're sorting
                // on.  If it's fetched, it has all the fields by definition.  If it's not, we check
                // sort field by sort field.
                if (!solnRoot->fetched()) {
                    bool sortCovered = true;
                    BSONObjIterator it(query.getParsed().getSort());
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
                sort->pattern = query.getParsed().getSort();
                sort->child.reset(solnRoot);
                solnRoot = sort;
            }
        }

        // Project the results.
        if (NULL != query.getProj()) {
            if (query.getProj()->requiresDocument()) {
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

    // Copied from db/index.h
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
    void QueryPlanner::plan(const CanonicalQuery& query, const vector<BSONObj>& indexKeyPatterns,
                            vector<QuerySolution*>* out) {
        cout << "Begin planning.\nquery = " << query.toString() << endl;
        // XXX: If pq.hasOption(QueryOption_OplogReplay) use FindingStartCursor equivalent which
        // must be translated into stages.

        // If the query requests a tailable cursor, the only solution is a collscan + filter with
        // tailable set on the collscan.  TODO: This is a policy departure.  Previously I think you
        // could ask for a tailable cursor and it just tried to give you one.  Now, we fail if we
        // can't provide one.  Is this what we want?
        if (query.getParsed().hasOption(QueryOption_CursorTailable)) {
            if (!hasNode(query.root(), MatchExpression::GEO_NEAR)) {
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
                out->push_back(makeCollectionScan(query, false));
                return;
            }
        }

        // NOR and NOT we can't handle well with indices.  If we see them here, they weren't
        // rewritten.  Just output a collscan for those.
        if (hasNode(query.root(), MatchExpression::NOT)
            || hasNode(query.root(), MatchExpression::NOR)) {

            // If there's a near predicate, we can't handle this.
            // TODO: Should canonicalized query detect this?
            if (hasNode(query.root(), MatchExpression::GEO_NEAR)) {
                warning() << "Can't handle NOT/NOR with GEO_NEAR";
                return;
            }
            out->push_back(makeCollectionScan(query, false));
            return;
        }

        // Figure out what fields we care about.
        unordered_set<string> fields;
        getFields(query.root(), &fields);

        // Filter our indices so we only look at indices that are over our predicates.
        vector<BSONObj> relevantIndices;

        // Hints require us to only consider the hinted index.
        BSONObj hintIndex = query.getParsed().getHint();

        // Snapshot is a form of a hint.  If snapshot is set, try to use _id index to make a real
        // plan.  If that fails, just scan the _id index.
        if (query.getParsed().isSnapshot()) {
            // Find the ID index in indexKeyPatterns.  It's our hint.
            for (size_t i = 0; i < indexKeyPatterns.size(); ++i) {
                if (isIdIndex(indexKeyPatterns[i])) {
                    hintIndex = indexKeyPatterns[i];
                    break;
                }
            }
        }

        if (!hintIndex.isEmpty()) {
            // TODO: make sure hintIndex exists in relevantIndices
            relevantIndices.clear();
            relevantIndices.push_back(hintIndex);

            // cout << "hint specified, restricting indices to " << hintIndex.toString() << endl;
        }
        else {
            findRelevantIndices(fields, indexKeyPatterns, &relevantIndices);
        }

        // Figure out how useful each index is to each predicate.
        // query.root() is now annotated with RelevantTag(s).
        rateIndices(query.root(), relevantIndices);

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
                QuerySolutionNode* solnRoot = buildIndexedDataAccess(rawTree, relevantIndices);
                if (NULL == solnRoot) { continue; }

                // This shouldn't ever fail.
                QuerySolution* soln = analyzeDataAccess(query, rawTree, solnRoot);
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

            // TODO: use simple bounds for ixscan once builder supports them
            BSONObjIterator it(isn->indexKeyPattern);
            while (it.more()) {
                isn->bounds.fields.push_back(
                        IndexBoundsBuilder::allValuesForField(it.next()));
            }

            // TODO: We may not need to do the fetch if the predicates in root are covered.  But
            // for now it's safe (though *maybe* slower).
            FetchNode* fetch = new FetchNode();
            fetch->filter = query.root()->shallowClone();
            fetch->child.reset(isn);

            QuerySolution* soln = analyzeDataAccess(query, fetch->filter, fetch);

            verify(NULL != soln);
            out->push_back(soln);
            cout << "using hinted index as scan, soln = " << soln->toString() << endl;
            return;
        }

        // If a sort order is requested, there may be an index that provides it, even if that
        // index is not over any predicates in the query.
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
                for (size_t i = 0; i < indexKeyPatterns.size(); ++i) {
                    const BSONObj& kp = indexKeyPatterns[i];
                    if (0 == kp.woCompare(query.getParsed().getSort())) {
                        IndexScanNode* isn = new IndexScanNode();
                        isn->indexKeyPattern = kp;

                        // TODO: use simple bounds for ixscan once builder supports them
                        BSONObjIterator it(isn->indexKeyPattern);
                        while (it.more()) {
                            isn->bounds.fields.push_back(
                                    IndexBoundsBuilder::allValuesForField(it.next()));
                        }

                        // TODO: We may not need to do the fetch if the predicates in root are covered.  But
                        // for now it's safe (though *maybe* slower).
                        FetchNode* fetch = new FetchNode();
                        fetch->filter = query.root()->shallowClone();
                        fetch->child.reset(isn);

                        QuerySolution* soln = analyzeDataAccess(query, fetch->filter, fetch);
                        verify(NULL != soln);
                        out->push_back(soln);
                        cout << "using index to provide sort, soln = " << soln->toString() << endl;
                        break;
                    }
                }
            }
        }

        // TODO: Do we always want to offer a collscan solution?
        if (!hasNode(query.root(), MatchExpression::GEO_NEAR)) {
            QuerySolution* collscan = makeCollectionScan(query, false);
            out->push_back(collscan);
            cout << "Outputting a collscan\n";
        }
    }

}  // namespace mongo
