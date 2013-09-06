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
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/pch.h"

namespace mongo {

    class FieldPath {
    public:

        /**
         * Constructor.
         *
         * @param fieldPath the dotted field path string or non empty pre-split vector.
         * The constructed object will have getPathLength() > 0.
         * Uassert if any component field names do not pass validation.
         */
        FieldPath(const string& fieldPath);
        FieldPath(const vector<string>& fieldPath);

        /**
          Get the number of path elements in the field path.

          @returns the number of path elements
         */
        size_t getPathLength() const;

        /**
          Get a particular path element from the path.

          @param i the zero based index of the path element.
          @returns the path element
         */
        const string& getFieldName(size_t i) const;

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
           Get the prefix string.

           @returns the prefix string
         */
        static const char *getPrefix();

        static const char prefix[];

        /**
         * A FieldPath like this but missing the first element (useful for recursion).
         * Precondition getPathLength() > 1.
         */
        FieldPath tail() const;

    private:
        /** Uassert if a field name does not pass validation. */
        static void uassertValidFieldName(const string& fieldName);

        /**
         * Push a new field name to the back of the vector of names comprising the field path.
         * Uassert if 'fieldName' does not pass validation.
         */
        void pushFieldName(const string& fieldName);

        vector<string> vFieldName;
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline size_t FieldPath::getPathLength() const {
        return vFieldName.size();
    }

    inline const string& FieldPath::getFieldName(size_t i) const {
        dassert(i < getPathLength());
        return vFieldName[i];
    }

    inline const char *FieldPath::getPrefix() {
        return prefix;
    }

}

