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

#include "mongo/db/query/index_tag.h"

#include "mongo/db/query/indexability.h"

#include <algorithm>
#include <limits>

namespace mongo {

    const size_t IndexTag::kNoIndex = std::numeric_limits<size_t>::max();

    void tagForSort(MatchExpression* tree) {
        if (!Indexability::nodeCanUseIndexOnOwnField(tree)) {
            size_t myTagValue = IndexTag::kNoIndex;
            for (size_t i = 0; i < tree->numChildren(); ++i) {
                MatchExpression* child = tree->getChild(i);
                tagForSort(child);
                IndexTag* childTag = static_cast<IndexTag*>(child->getTag());
                if (NULL != childTag) {
                    myTagValue = std::min(myTagValue, childTag->index);
                }
            }
            if (myTagValue != IndexTag::kNoIndex) {
                tree->setTag(new IndexTag(myTagValue));
            }
        }
    }

    bool TagComparison(const MatchExpression* lhs, const MatchExpression* rhs) {
        IndexTag* lhsTag = static_cast<IndexTag*>(lhs->getTag());
        size_t lhsValue = (NULL == lhsTag) ? IndexTag::kNoIndex : lhsTag->index;
        IndexTag* rhsTag = static_cast<IndexTag*>(rhs->getTag());
        size_t rhsValue = (NULL == rhsTag) ? IndexTag::kNoIndex : rhsTag->index;

        // First, order on indices.
        if (lhsValue != rhsValue) {
            // This relies on kNoIndex being larger than every other possible index.
            return lhsValue < rhsValue;
        }

        // Next, order on fields.
        int cmp = lhs->path().compare(rhs->path());
        if (0 != cmp) {
            return 0;
        }

        // Finally, order on expression type.
        return lhs->matchType() < rhs->matchType();
    }

    void sortUsingTags(MatchExpression* tree) {
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            sortUsingTags(tree->getChild(i));
        }
        std::vector<MatchExpression*>* children = tree->getChildVector();
        if (NULL != children) {
            std::sort(children->begin(), children->end(), TagComparison);
        }
    }

} // namespace mongo
