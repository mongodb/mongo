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

#pragma once

#include <vector>

#include "mongo/bson/util/builder.h"
#include "mongo/db/matcher/expression.h"


namespace mongo {

    // output from enumerator to query planner
    class IndexTag : public MatchExpression::TagData {
    public:
        static const size_t kNoIndex;

        IndexTag() : index(kNoIndex) {}
        IndexTag(size_t i) : index(i) { }

        virtual ~IndexTag() { }

        virtual void debugString(StringBuilder* builder) const {
            *builder << " || Selected Index #" << index;
        }

        virtual MatchExpression::TagData* clone() const {
            return new IndexTag(index);
        }

        // What index should we try to use for this leaf?
        size_t index;
    };

    // used internally
    class RelevantTag : public MatchExpression::TagData {
    public:
        RelevantTag() { }

        std::vector<size_t> first;
        std::vector<size_t> notFirst;

        virtual void debugString(StringBuilder* builder) const {
            *builder << "First: ";
            for (size_t i = 0; i < first.size(); ++i) {
                *builder << first[i] << " ";
            }
            *builder << "notFirst: ";
            for (size_t i = 0; i < notFirst.size(); ++i) {
                *builder << notFirst[i] << " ";
            }
        }

        virtual MatchExpression::TagData* clone() const {
            RelevantTag* ret = new RelevantTag();
            ret->first = first;
            ret->notFirst = first;
            return ret;
        }
    };

    /**
     * Tags each node of the tree with the lowest numbered index that the sub-tree rooted at that
     * node uses.
     *
     * Nodes that satisfy Indexability::nodeCanUseIndexOnOwnField are already tagged if there
     * exists an index that that node can use.
     */
    void tagForSort(MatchExpression* tree);

    /**
     * Sorts the tree using its IndexTag(s). Nodes that use the same index are adjacent to one
     * another.
     */
    void sortUsingTags(MatchExpression* tree);

} // namespace mongo
