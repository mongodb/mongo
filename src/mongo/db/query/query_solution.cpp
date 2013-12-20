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

namespace mongo {

    string QuerySolutionNode::toString() const {
        stringstream ss;
        appendToString(&ss, 0);
        return ss.str();
    }

    // static
    void QuerySolutionNode::addIndent(stringstream* ss, int level) {
        for (int i = 0; i < level; ++i) {
            *ss << "---";
        }
    }

    void QuerySolutionNode::addCommon(stringstream* ss, int indent) const {
        addIndent(ss, indent + 1);
        *ss << "fetched = " << fetched() << endl;
        addIndent(ss, indent + 1);
        *ss << "sortedByDiskLoc = " << sortedByDiskLoc() << endl;
        addIndent(ss, indent + 1);
        *ss << "getSort = [";
        for (BSONObjSet::const_iterator it = getSort().begin(); it != getSort().end(); it++) {
            *ss << it->toString() << ", ";
        }
        *ss << "]" << endl;
    }

    //
    // TextNode
    //

    void TextNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "TEXT\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << _indexKeyPattern.toString() << endl;
        addIndent(ss, indent + 1);
        *ss << "query = " << _query << endl;
        addIndent(ss, indent + 1);
        *ss << "language = " << _language << endl;
        addCommon(ss, indent);
    }

    //
    // CollectionScanNode
    //

    CollectionScanNode::CollectionScanNode() : tailable(false), direction(1), maxScan(0) { }

    void CollectionScanNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "COLLSCAN\n";
        addIndent(ss, indent + 1);
        *ss <<  "ns = " << name << endl;
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString();
        }
        addCommon(ss, indent);
    }

    //
    // AndHashNode
    //

    AndHashNode::AndHashNode() { }

    AndHashNode::~AndHashNode() { }

    void AndHashNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "AND_HASH\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString() << endl;
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

    //
    // AndSortedNode
    //

    AndSortedNode::AndSortedNode() { }

    AndSortedNode::~AndSortedNode() { }

    void AndSortedNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "AND_SORTED\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString() << endl;
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

    //
    // OrNode
    //

    OrNode::OrNode() : dedup(true) { }

    OrNode::~OrNode() { }

    void OrNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "OR\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString() << endl;
        }
        addCommon(ss, indent);
        for (size_t i = 0; i < children.size(); ++i) {
            addIndent(ss, indent + 1);
            *ss << "Child " << i << ":\n";
            children[i]->appendToString(ss, indent + 2);
            *ss << endl;
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

    //
    // MergeSortNode
    //

    MergeSortNode::MergeSortNode() : dedup(true) { }

    MergeSortNode::~MergeSortNode() { }

    void MergeSortNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "MERGE_SORT\n";
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString() << endl;
        }
        addCommon(ss, indent);
        for (size_t i = 0; i < children.size(); ++i) {
            addIndent(ss, indent + 1);
            *ss << "Child " << i << ":\n";
            children[i]->appendToString(ss, indent + 2);
            *ss << endl;
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

    //
    // FetchNode
    //

    FetchNode::FetchNode() { }

    void FetchNode::appendToString(stringstream* ss, int indent) const {
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
        *ss << "Child:" << endl;
        children[0]->appendToString(ss, indent + 2);
    }

    //
    // IndexScanNode
    //

    IndexScanNode::IndexScanNode()
        : indexIsMultiKey(false), limit(0), direction(1), maxScan(0), addKeyMetadata(false) { }

    void IndexScanNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "IXSCAN\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern << endl;
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter= " << filter->toString() << endl;
        }
        addIndent(ss, indent + 1);
        *ss << "direction = " << direction << endl;
        addIndent(ss, indent + 1);
        *ss << "bounds = " << bounds.toString() << endl;
        addIndent(ss, indent + 1);
        *ss << "fetched = " << fetched() << endl;
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
        BSONObjBuilder prefixBob;
        while (it.more()) {
            BSONElement elt = it.next();
            // XXX string slowness.  fix when bounds are stringdata not string.
            if (equalityFields.end() == equalityFields.find(string(elt.fieldName()))) {
                prefixBob.append(elt);
                // This field isn't a point interval, can't drop.
                break;
            }
        }

        while (it.more()) {
            prefixBob.append(it.next());
        }

        // If we have an index {a:1} and an equality on 'a' don't append an empty sort order.
        BSONObj filterPointsObj = prefixBob.obj();
        if (!filterPointsObj.isEmpty()) {
            _sorts.insert(filterPointsObj);
        }
    }

    //
    // ProjectionNode
    //

    void ProjectionNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "PROJ\n";
        addIndent(ss, indent + 1);
        *ss << "proj = " << projection.toString() << endl;
        addCommon(ss, indent);
        *ss << "Child:" << endl;
        children[0]->appendToString(ss, indent + 2);
    }

    //
    // SortNode
    //

    void SortNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "SORT\n";
        addIndent(ss, indent + 1);
        *ss << "pattern = " << pattern.toString() << endl;
        addIndent(ss, indent + 1);
        *ss << "query for bounds = " << query.toString() << endl;
        addIndent(ss, indent + 1);
        *ss << "limit = " << limit << endl;
        addCommon(ss, indent);
        *ss << "Child:" << endl;
        children[0]->appendToString(ss, indent + 2);
    }

    //
    // LimitNode
    //
    

    void LimitNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "LIMIT\n";
        addIndent(ss, indent + 1);
        *ss << "limit = " << limit << endl;
        addIndent(ss, indent + 1);
        addCommon(ss, indent);
        *ss << "Child:" << endl;
        children[0]->appendToString(ss, indent + 2);
    }

    //
    // SkipNode
    //

    void SkipNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "SKIP\n";
        addIndent(ss, indent + 1);
        *ss << "skip= " << skip << endl;
        addCommon(ss, indent);
        *ss << "Child:" << endl;
        children[0]->appendToString(ss, indent + 2);
    }

    //
    // GeoNear2DNode
    //

    void GeoNear2DNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "GEO_NEAR_2D\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern.toString() << endl;
        addCommon(ss, indent);
        *ss << "nearQuery = " << nq.toString() << endl;
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString();
        }
    }

    //
    // GeoNear2DSphereNode
    //

    void GeoNear2DSphereNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "GEO_NEAR_2DSPHERE\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern.toString() << endl;
        addCommon(ss, indent);
        *ss << "baseBounds = " << baseBounds.toString() << endl;
        addIndent(ss, indent + 1);
        *ss << "nearQuery = " << nq.toString() << endl;
        if (NULL != filter) {
            addIndent(ss, indent + 1);
            *ss << " filter = " << filter->toString();
        }
    }

    //
    // Geo2DNode
    //

    void Geo2DNode::appendToString(stringstream* ss, int indent) const {
        addIndent(ss, indent);
        *ss << "GEO_2D\n";
        addIndent(ss, indent + 1);
        *ss << "keyPattern = " << indexKeyPattern.toString() << endl;
        addCommon(ss, indent);
    }

    bool Geo2DNode::hasField(const string& field) const {
        BSONObjIterator it(indexKeyPattern);
        while (it.more()) {
            if (field == it.next().fieldName()) {
                return true;
            }
        }
        return false;
    }

    //
    // ShardingFilterNode
    //

    void ShardingFilterNode::appendToString(stringstream* ss, int indent) const {
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
        *ss << "Child:" << endl;
        children[0]->appendToString(ss, indent + 2);
    }

}  // namespace mongo
