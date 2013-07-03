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

namespace mongo {

    class AndCommon {
    public:
        /**
         * If src has any data dest doesn't, add that data to dest.
         */
        static void mergeFrom(WorkingSetMember* dest, WorkingSetMember* src) {
            verify(dest->hasLoc());
            verify(src->hasLoc());
            verify(dest->loc == src->loc);

            // This is N^2 but N is probably pretty small.  Easy enough to revisit.
            // Merge key data.
            for (size_t i = 0; i < src->keyData.size(); ++i) {
                bool found = false;
                for (size_t j = 0; j < dest->keyData.size(); ++j) {
                    if (dest->keyData[j].indexKeyPattern == src->keyData[i].indexKeyPattern) {
                        found = true;
                        break;
                    }
                }
                if (!found) { dest->keyData.push_back(src->keyData[i]); }
            }
        }
    };

}  // namespace mongo

