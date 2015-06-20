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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/query/index_tag.h"

#include "mongo/db/query/indexability.h"

#include <algorithm>
#include <limits>

namespace mongo {

// TODO: Move out of the enumerator and into the planner.

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
    size_t lhsPos = (NULL == lhsTag) ? IndexTag::kNoIndex : lhsTag->pos;

    IndexTag* rhsTag = static_cast<IndexTag*>(rhs->getTag());
    size_t rhsValue = (NULL == rhsTag) ? IndexTag::kNoIndex : rhsTag->index;
    size_t rhsPos = (NULL == rhsTag) ? IndexTag::kNoIndex : rhsTag->pos;

    // First, order on indices.
    if (lhsValue != rhsValue) {
        // This relies on kNoIndex being larger than every other possible index.
        return lhsValue < rhsValue;
    }

    // Next, order so that if there's a GEO_NEAR it's first.
    if (MatchExpression::GEO_NEAR == lhs->matchType()) {
        return true;
    } else if (MatchExpression::GEO_NEAR == rhs->matchType()) {
        return false;
    }

    // Ditto text.
    if (MatchExpression::TEXT == lhs->matchType()) {
        return true;
    } else if (MatchExpression::TEXT == rhs->matchType()) {
        return false;
    }

    // Next, order so that the first field of a compound index appears first.
    if (lhsPos != rhsPos) {
        return lhsPos < rhsPos;
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

}  // namespace mongo
