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
 */

#include "mongo/db/query/plan_enumerator.h"

#include <set>

#include "mongo/db/query/index_tag.h"

namespace mongo {

    PlanEnumerator::PlanEnumerator(MatchExpression* root,
                                   const PredicateMap* pm,
                                   const vector<BSONObj>* indices)
        : _root(root)
        , _pm(*pm)
        , _indices(*indices) {}

    Status PlanEnumerator::init() {
        if (_pm.size() == 0) {
            return Status(ErrorCodes::BadValue, "Cannot enumerate query without predicates map");
        }

        if (_indices.size() == 0) {
            return Status(ErrorCodes::BadValue, "Cannot enumerate indexed plans with no indices");
        }

        // See "navigation state" assumptions on this class's header.
        const set<RelevantIndex>& indexes = _pm.begin()->second.relevant;
        const vector<MatchExpression*>& nodes = _pm.begin()->second.nodes;

        verify(indexes.size());

        IndexInfo indexInfo;
        indexInfo.index = indexes.begin()->index;
        indexInfo.node = *nodes.begin();
        _indexes.push_back(indexInfo);

        // Prepare to return the first plan.
        _iter = 0;

        return Status::OK();
    }

    bool PlanEnumerator::getNext(MatchExpression** tree) {
        dassert(_indexes.size());

        // If we have used all indices that are useful in any number predicates, there's
        // nothing left to do.
        if (_iter == _indexes.size()) {
            return false;
        }

        // Annotate in the output tree, which nodes could use the index we're currently
        // working with here.
        int indexNum = _indexes[_iter].index;
        while (_iter < _indexes.size() && _indexes[_iter].index == indexNum) {

            IndexTag* indexTag = new IndexTag(indexNum);
            MatchExpression* node = _indexes[_iter].node;
            node->setTag(indexTag);

            ++_iter;
        }

        // XXX put the tree back the way it was.
        // XXX if we're not really manipulating the clone this is wasted work.
        MatchExpression* ret = _root->shallowClone();
        _root->resetTag();

        *tree = ret;
        return true;
    }

} // namespace mongo
