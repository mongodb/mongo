/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "pch.h"

namespace mongo {

    class FieldPath {
    public:
        virtual ~FieldPath();

        /**
           Constructor.

           @param fieldPath the dotted field path string
         */
        FieldPath(const string &fieldPath);

        /**
           Constructor.
        */
        FieldPath();

        /**
          Get the number of path elements in the field path.

          @returns the number of path elements
         */
        size_t getPathLength() const;

        /**
          Get a particular path element from the path.

          @param i the index of the path element
          @returns the path element
         */
        string getFieldName(size_t i) const;

        /**
          Get the full path.

          @param fieldPrefix whether or not to include the field prefix
          @returns the complete field path
         */
        string getPath(bool fieldPrefix) const;

        /**
          Write the full path.

          @param outStream where to write the path to
          @param fieldPrefix whether or not to include the field prefix
        */
        void writePath(ostream &outStream, bool fieldPrefix) const;

        /**
           Assignment operator.

           @param rRHS right hand side of the assignment
        */
        FieldPath &operator=(const FieldPath &rRHS);

        /**
           Get the prefix string.

           @returns the prefix string
         */
        static const char *getPrefix();

        static const char prefix[];

    private:
        vector<string> vFieldName;
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline size_t FieldPath::getPathLength() const {
        return vFieldName.size();
    }

    inline string FieldPath::getFieldName(size_t i) const {
        return vFieldName[i];
    }

    inline const char *FieldPath::getPrefix() {
        return prefix;
    }

}

