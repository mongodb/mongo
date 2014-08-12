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

#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/matcher/expression_geo.h"

namespace mongo {

    string QuerySolutionNode::toString() const {
        mongoutils::str::stream ss;
        appendToString(&ss, 0);
        return ss;
    }

    // static
    void QuerySolutionNode::addIndent(mongoutils::str::stream* ss, int level) {
        for (int i = 0; i < level; ++i) {
            *ss << "---";
        }
    }

    void QuerySolutionNode::addCommon(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent + 1);
        *ss << "fetched = " << fetched() << '\n';
        addIndent(ss, indent + 1);
        *ss << "sortedByDiskLoc = " << sortedByDiskLoc() << '\n';
        addIndent(ss, indent + 1);
        *ss << "getSort = [";
        for (BSONObjSet::const_iterator it = getSort().begin(); it != getSort().end(); it++) {
            *ss << it->toString() << ", ";
        }
        *ss << "]" << '\n';
    }

    //
    // TextNode
    //

    void TextNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "TEXT\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern.toString() << '\n';
        addIndent(ss, indent + 1);
        *ss << "query = " << query << '\n';
        addIndent(ss, indent + 1);
        *ss << "language = " << language << '\n';
        addIndent(ss, indent + 1);
        *ss << "indexPrefix = " << indexPrefix.toString() << '\n';
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString();
        }
        addCommon(ss, indent);
    }

    QuerySolutionNode* TextNode::clone() const {
        TextNode* copy = new TextNode();
        cloneBaseData(copy);

        copy->_sort = this->_sort;
        copy->indexKeyPattern = this->indexKeyPattern;
        copy->query = this->query;
        copy->language = this->language;
        copy->indexPrefix = this->indexPrefix;

        return copy;
    }

    //
    // CollectionScanNode
    //

    CollectionScanNode::CollectionScanNode() : tailable(false), direction(1), maxScan(0) { }

    void CollectionScanNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "COLLSCAN\n";
        addIndent(ss, indent + 1);
        *ss <<  "ns = " << name << '\n';
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << "filter = " << filter->toString();
        }
        addCommon(ss, indent);
    }

    QuerySolutionNode* CollectionScanNode::clone() const {
        CollectionScanNode* copy = new CollectionScanNode();
        cloneBaseData(copy);

        copy->_sort = this->_sort;
        copy->name = this->name;
        copy->tailable = this->tailable;
        copy->direction = this->direction;
        copy->maxScan = this->maxScan;

        return copy;
    }

    //
    // AndHashNode
    //

    AndHashNode::AndHashNode() { }

    AndHashNode::~AndHashNode() { }

    void AndHashNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "AND_HASH\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString() << '\n';
        }
        addCommon(ss, indent);
        for (size_t i = 0; i < children.size(); ++i) {
            addIndent(ss, indent + 1);
            *ss << "Child " << i << ":\n";
            children[i]->appendToString(ss, indent + 1);
        }
    }

    bool AndHashNode::fetched() const {
        // Any WSM output from this stage came from all children stages.  If any child provides
        // fetched data, we merge that fetched data into the WSM we output.
        for (size_t i = 0; i < children.size(); ++i) {
            if (children[i]->fetched()) {
                return true;
            }
        }
        return false;
    }

    bool AndHashNode::hasField(const string& field) const {
        // Any WSM output from this stage came from all children stages.  Therefore we have all
        // fields covered in our children.
        for (size_t i = 0; i < children.size(); ++i) {
            if (children[i]->hasField(field)) {
                return true;
            }
        }
        return false;
    }

    QuerySolutionNode* AndHashNode::clone() const {
        AndHashNode* copy = new AndHashNode();
        cloneBaseData(copy);

        copy->_sort = this->_sort;

        return copy;
    }

    //
    // AndSortedNode
    //

    AndSortedNode::AndSortedNode() { }

    AndSortedNode::~AndSortedNode() { }

    void AndSortedNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "AND_SORTED\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString() << '\n';
        }
        addCommon(ss, indent);
        for (size_t i = 0; i < children.size(); ++i) {
            addIndent(ss, indent + 1);
            *ss << "Child " << i << ":\n";
            children[i]->appendToString(ss, indent + 1);
        }
    }

    bool AndSortedNode::fetched() const {
        // Any WSM output from this stage came from all children stages.  If any child provides
        // fetched data, we merge that fetched data into the WSM we output.
        for (size_t i = 0; i < children.size(); ++i) {
            if (children[i]->fetched()) {
                return true;
            }
        }
        return false;
    }

    bool AndSortedNode::hasField(const string& field) const {
        // Any WSM output from this stage came from all children stages.  Therefore we have all
        // fields covered in our children.
        for (size_t i = 0; i < children.size(); ++i) {
            if (children[i]->hasField(field)) {
                return true;
            }
        }
        return false;
    }

    QuerySolutionNode* AndSortedNode::clone() const {
        AndSortedNode* copy = new AndSortedNode();
        cloneBaseData(copy);

        copy->_sort = this->_sort;

        return copy;
    }

    //
    // OrNode
    //

    OrNode::OrNode() : dedup(true) { }

    OrNode::~OrNode() { }

    void OrNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "OR\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString() << '\n';
        }
        addCommon(ss, indent);
        for (size_t i = 0; i < children.size(); ++i) {
            addIndent(ss, indent + 1);
            *ss << "Child " << i << ":\n";
            children[i]->appendToString(ss, indent + 2);
            *ss << '\n';
        }
    }

    bool OrNode::fetched() const {
        // Any WSM output from this stage came exactly one child stage.  Given that we don't know
        // what child stage it came from, we require that all children provide fetched data in order
        // to guarantee that our output is fetched.
        for (size_t i = 0; i < children.size(); ++i) {
            if (!children[i]->fetched()) {
                return false;
            }
        }
        return true;
    }

    /**
     * Any WSM output from this stage came from exactly one child stage.  Therefore, if
     * we want to guarantee that any output has a certain field, all of our children must
     * have that field.
     */
    bool OrNode::hasField(const string& field) const {
        for (size_t i = 0; i < children.size(); ++i) {
            if (!children[i]->hasField(field)) {
                return false;
            }
        }
        return true;
    }

    QuerySolutionNode* OrNode::clone() const {
        OrNode* copy = new OrNode();
        cloneBaseData(copy);

        copy->_sort = this->_sort;
        copy->dedup = this->dedup;

        return copy;
    }

    //
    // MergeSortNode
    //

    MergeSortNode::MergeSortNode() : dedup(true) { }

    MergeSortNode::~MergeSortNode() { }

    void MergeSortNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "MERGE_SORT\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString() << '\n';
        }
        addCommon(ss, indent);
        for (size_t i = 0; i < children.size(); ++i) {
            addIndent(ss, indent + 1);
            *ss << "Child " << i << ":\n";
            children[i]->appendToString(ss, indent + 2);
            *ss << '\n';
        }
    }

    bool MergeSortNode::fetched() const {
        // Any WSM output from this stage came exactly one child stage.  Given that we don't know
        // what child stage it came from, we require that all children provide fetched data in order
        // to guarantee that our output is fetched.
        for (size_t i = 0; i < children.size(); ++i) {
            if (!children[i]->fetched()) {
                return false;
            }
        }
        return true;
    }

    /**
     * Any WSM output from this stage came from exactly one child stage.  Therefore, if
     * we want to guarantee that any output has a certain field, all of our children must
     * have that field.
     */
    bool MergeSortNode::hasField(const string& field) const {
        for (size_t i = 0; i < children.size(); ++i) {
            if (!children[i]->hasField(field)) {
                return false;
            }
        }
        return true;
    }

    QuerySolutionNode* MergeSortNode::clone() const {
        MergeSortNode* copy = new MergeSortNode();
        cloneBaseData(copy);

        copy->_sorts = this->_sorts;
        copy->dedup = this->dedup;
        copy->sort = this->sort;

        return copy;
    }

    //
    // FetchNode
    //

    FetchNode::FetchNode() { }

    void FetchNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "FETCH\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            StringBuilder sb;
            *ss << "filter:\n";
            filter->debugString(sb, indent + 2);
            *ss << sb.str();
        }
        addCommon(ss, indent);
        addIndent(ss, indent + 1);
        *ss << "Child:" << '\n';
        children[0]->appendToString(ss, indent + 2);
    }

    QuerySolutionNode* FetchNode::clone() const {
        FetchNode* copy = new FetchNode();
        cloneBaseData(copy);

        copy->_sorts = this->_sorts;

        return copy;
    }

    //
    // IndexScanNode
    //

    IndexScanNode::IndexScanNode()
        : indexIsMultiKey(false), direction(1), maxScan(0), addKeyMetadata(false) { }

    void IndexScanNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "IXSCAN\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern << '\n';
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << "filter = " << filter->toString();
        }
        addIndent(ss, indent + 1);
        *ss << "direction = " << direction << '\n';
        addIndent(ss, indent + 1);
        *ss << "bounds = " << bounds.toString() << '\n';
        addCommon(ss, indent);
    }

    bool IndexScanNode::hasField(const string& field) const {
        // There is no covering in a multikey index because you don't know whether or not the field
        // in the key was extracted from an array in the original document.
        if (indexIsMultiKey) { return false; }

        BSONObjIterator it(indexKeyPattern);
        while (it.more()) {
            if (field == it.next().fieldName()) {
                return true;
            }
        }
        return false;
    }

    bool IndexScanNode::sortedByDiskLoc() const {
        // Indices use DiskLoc as an additional key after the actual index key.
        // Therefore, if we're only examining one index key, the output is sorted
        // by DiskLoc.

        // If it's a simple range query, it's easy to determine if the range is a point.
        if (bounds.isSimpleRange) {
            return 0 == bounds.startKey.woCompare(bounds.endKey, indexKeyPattern);
        }

        // If it's a more complex bounds query, we make sure that each field is a point.
        for (size_t i = 0; i < bounds.fields.size(); ++i) {
            const OrderedIntervalList& oil = bounds.fields[i];
            if (1 != oil.intervals.size()) {
                return false;
            }
            const Interval& interval = oil.intervals[0];
            if (0 != interval.start.woCompare(interval.end, false)) {
                return false;
            }
        }

        return true;
    }

    void IndexScanNode::computeProperties() {
        _sorts.clear();

        BSONObj sortPattern;
        {
            BSONObjBuilder sortBob;
            BSONObj normalizedIndexKeyPattern(LiteParsedQuery::normalizeSortOrder(indexKeyPattern));
            BSONObjIterator it(normalizedIndexKeyPattern);
            while (it.more()) {
                BSONElement elt = it.next();
                // Zero is returned if elt is not a number.  This happens when elt is hashed or
                // 2dsphere, our two projection indices.  We want to drop those from the sort
                // pattern.
                int val = elt.numberInt() * direction;
                if (0 != val) {
                    sortBob.append(elt.fieldName(), val);
                }
            }
            sortPattern = sortBob.obj();
        }
        _sorts.insert(sortPattern);

        const int nFields = sortPattern.nFields();
        if (nFields > 1) {
            // We're sorted not only by sortPattern but also by all prefixes of it.
            for (int i = 0; i < nFields; ++i) {
                // Make obj out of fields [0,i]
                BSONObjIterator it(sortPattern);
                BSONObjBuilder prefixBob;
                for (int j = 0; j <= i; ++j) {
                    prefixBob.append(it.next());
                }
                _sorts.insert(prefixBob.obj());
            }
        }

        // If we are using the index {a:1, b:1} to answer the predicate {a: 10}, it's sorted
        // both by the index key pattern and by the pattern {b: 1}.

        // See if there are any fields with equalities for bounds.  We can drop these
        // from any sort orders created.
        set<string> equalityFields;
        if (!bounds.isSimpleRange) {
            // Figure out how many fields are point intervals.
            for (size_t i = 0; i < bounds.fields.size(); ++i) {
                const OrderedIntervalList& oil = bounds.fields[i];
                if (oil.intervals.size() != 1) {
                    continue;
                }
                const Interval& ival = oil.intervals[0];
                if (!ival.isPoint()) {
                    continue;
                }
                equalityFields.insert(oil.name);
            }
        }

        if (equalityFields.empty()) {
            return;
        }

        // TODO: Each field in equalityFields could be dropped from the sort order since it is
        // a point interval.  The full set of sort orders is as follows:
        // For each sort in _sorts:
        //    For each drop in powerset(equalityFields):
        //        Remove fields in 'drop' from 'sort' and add resulting sort to output.

        // Since this involves a powerset, we only remove point intervals that the prior sort
        // planning code removed, namely the contiguous prefix of the key pattern.
        BSONObjIterator it(sortPattern);
        BSONObjBuilder suffixBob;
        while (it.more()) {
            BSONElement elt = it.next();
            // TODO: string slowness.  fix when bounds are stringdata not string.
            if (equalityFields.end() == equalityFields.find(string(elt.fieldName()))) {
                suffixBob.append(elt);
                // This field isn't a point interval, can't drop.
                break;
            }
        }

        while (it.more()) {
            suffixBob.append(it.next());
        }

        // We've found the suffix following the contiguous prefix of equality fields.
        //   Ex. For index {a: 1, b: 1, c: 1, d: 1} and query {a: 3, b: 5}, this suffix
        //   of the key pattern is {c: 1, d: 1}.
        //
        // Now we have to add all prefixes of this suffix as possible sort orders.
        //   Ex. Continuing the example from above, we have to include sort orders
        //   {c: 1} and {c: 1, d: 1}.
        BSONObj filterPointsObj = suffixBob.obj();
        for (int i = 0; i < filterPointsObj.nFields(); ++i) {
            // Make obj out of fields [0,i]
            BSONObjIterator it(filterPointsObj);
            BSONObjBuilder prefixBob;
            for (int j = 0; j <= i; ++j) {
                prefixBob.append(it.next());
            }
            _sorts.insert(prefixBob.obj());
        }
    }

    QuerySolutionNode* IndexScanNode::clone() const {
        IndexScanNode* copy = new IndexScanNode();
        cloneBaseData(copy);

        copy->_sorts = this->_sorts;
        copy->indexKeyPattern = this->indexKeyPattern;
        copy->indexIsMultiKey = this->indexIsMultiKey;
        copy->direction = this->direction;
        copy->maxScan = this->maxScan;
        copy->addKeyMetadata = this->addKeyMetadata;
        copy->bounds = this->bounds;

        return copy;
    }

    //
    // ProjectionNode
    //

    void ProjectionNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "PROJ\n";
        addIndent(ss, indent + 1);
        *ss << "proj = " << projection.toString() << '\n';
        addIndent(ss, indent + 1);
        if (DEFAULT == projType) {
            *ss << "type = DEFAULT\n";
        }
        else if (COVERED_ONE_INDEX == projType) {
            *ss << "type = COVERED_ONE_INDEX\n";
        }
        else {
            invariant(SIMPLE_DOC == projType);
            *ss << "type = SIMPLE_DOC\n";
        }
        addCommon(ss, indent);
        addIndent(ss, indent + 1);
        *ss << "Child:" << '\n';
        children[0]->appendToString(ss, indent + 2);
    }

    QuerySolutionNode* ProjectionNode::clone() const {
        ProjectionNode* copy = new ProjectionNode();
        cloneBaseData(copy);

        copy->_sorts = this->_sorts;
        copy->fullExpression = this->fullExpression;

        // This MatchExpression* is owned by the canonical query, not by the
        // ProjectionNode. Just copying the pointer is fine.
        copy->projection = this->projection;

        return copy;
    }

    //
    // SortNode
    //

    void SortNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "SORT\n";
        addIndent(ss, indent + 1);
        *ss << "pattern = " << pattern.toString() << '\n';
        addIndent(ss, indent + 1);
        *ss << "query for bounds = " << query.toString() << '\n';
        addIndent(ss, indent + 1);
        *ss << "limit = " << limit << '\n';
        addCommon(ss, indent);
        addIndent(ss, indent + 1);
        *ss << "Child:" << '\n';
        children[0]->appendToString(ss, indent + 2);
    }

    QuerySolutionNode* SortNode::clone() const {
        SortNode* copy = new SortNode();
        cloneBaseData(copy);

        copy->_sorts = this->_sorts;
        copy->pattern = this->pattern;
        copy->query = this->query;
        copy->limit = this->limit;

        return copy;
    }

    //
    // LimitNode
    //


    void LimitNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "LIMIT\n";
        addIndent(ss, indent + 1);
        *ss << "limit = " << limit << '\n';
        addIndent(ss, indent + 1);
        addCommon(ss, indent);
        addIndent(ss, indent + 1);
        *ss << "Child:" << '\n';
        children[0]->appendToString(ss, indent + 2);
    }

    QuerySolutionNode* LimitNode::clone() const {
        LimitNode* copy = new LimitNode();
        cloneBaseData(copy);

        copy->limit = this->limit;

        return copy;
    }

    //
    // SkipNode
    //

    void SkipNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "SKIP\n";
        addIndent(ss, indent + 1);
        *ss << "skip= " << skip << '\n';
        addCommon(ss, indent);
        addIndent(ss, indent + 1);
        *ss << "Child:" << '\n';
        children[0]->appendToString(ss, indent + 2);
    }

    QuerySolutionNode* SkipNode::clone() const {
        SkipNode* copy = new SkipNode();
        cloneBaseData(copy);

        copy->skip = this->skip;

        return copy;
    }

    //
    // GeoNear2DNode
    //

    void GeoNear2DNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "GEO_NEAR_2D\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern.toString() << '\n';
        addCommon(ss, indent);
        *ss << "nearQuery = " << nq->toString() << '\n';
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString();
        }
    }

    QuerySolutionNode* GeoNear2DNode::clone() const {
        GeoNear2DNode* copy = new GeoNear2DNode();
        cloneBaseData(copy);

        copy->_sorts = this->_sorts;
        copy->nq = this->nq;
        copy->baseBounds = this->baseBounds;
        copy->indexKeyPattern = this->indexKeyPattern;
        copy->addPointMeta = this->addPointMeta;
        copy->addDistMeta = this->addDistMeta;

        return copy;
    }

    //
    // GeoNear2DSphereNode
    //

    void GeoNear2DSphereNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "GEO_NEAR_2DSPHERE\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern.toString() << '\n';
        addCommon(ss, indent);
        *ss << "baseBounds = " << baseBounds.toString() << '\n';
        addIndent(ss, indent + 1);
        *ss << "nearQuery = " << nq->toString() << '\n';
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString();
        }
    }

    QuerySolutionNode* GeoNear2DSphereNode::clone() const {
        GeoNear2DSphereNode* copy = new GeoNear2DSphereNode();
        cloneBaseData(copy);

        copy->_sorts = this->_sorts;
        copy->nq = this->nq;
        copy->baseBounds = this->baseBounds;
        copy->indexKeyPattern = this->indexKeyPattern;
        copy->addPointMeta = this->addPointMeta;
        copy->addDistMeta = this->addDistMeta;

        return copy;
    }

    //
    // ShardingFilterNode
    //

    void ShardingFilterNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "SHARDING_FILTER\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            StringBuilder sb;
            *ss << "filter:\n";
            filter->debugString(sb, indent + 2);
            *ss << sb.str();
        }
        addCommon(ss, indent);
        addIndent(ss, indent + 1);
        *ss << "Child:" << '\n';
        children[0]->appendToString(ss, indent + 2);
    }

    QuerySolutionNode* ShardingFilterNode::clone() const {
        ShardingFilterNode* copy = new ShardingFilterNode();
        cloneBaseData(copy);
        return copy;
    }

    //
    // KeepMutationsNode
    //

    void KeepMutationsNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "KEEP_MUTATIONS\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            StringBuilder sb;
            *ss << "filter:\n";
            filter->debugString(sb, indent + 2);
            *ss << sb.str();
        }
        addCommon(ss, indent);
        addIndent(ss, indent + 1);
        *ss << "Child:" << '\n';
        children[0]->appendToString(ss, indent + 2);
    }

    QuerySolutionNode* KeepMutationsNode::clone() const {
        KeepMutationsNode* copy = new KeepMutationsNode();
        cloneBaseData(copy);

        copy->sorts = this->sorts;

        return copy;
    }

    //
    // DistinctNode
    //

    void DistinctNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "DISTINCT\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern << '\n';
        addIndent(ss, indent + 1);
        *ss << "direction = " << direction << '\n';
        addIndent(ss, indent + 1);
        *ss << "bounds = " << bounds.toString() << '\n';
    }

    QuerySolutionNode* DistinctNode::clone() const {
        DistinctNode* copy = new DistinctNode();
        cloneBaseData(copy);

        copy->sorts = this->sorts;
        copy->indexKeyPattern = this->indexKeyPattern;
        copy->direction = this->direction;
        copy->bounds = this->bounds;
        copy->fieldNo = this->fieldNo;

        return copy;
    }

    //
    // CountNode
    //

    void CountNode::appendToString(mongoutils::str::stream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "COUNT\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern << '\n';
        addIndent(ss, indent + 1);
        *ss << "startKey = " << startKey << '\n';
        addIndent(ss, indent + 1);
        *ss << "endKey = " << endKey << '\n';
    }

    QuerySolutionNode* CountNode::clone() const {
        CountNode* copy = new CountNode();
        cloneBaseData(copy);

        copy->sorts = this->sorts;
        copy->indexKeyPattern = this->indexKeyPattern;
        copy->startKey = this->startKey;
        copy->startKeyInclusive = this->startKeyInclusive;
        copy->endKey = this->endKey;
        copy->endKeyInclusive = this->endKeyInclusive;

        return copy;
    }

}  // namespace mongo
