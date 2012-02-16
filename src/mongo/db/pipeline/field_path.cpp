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

#include "pch.h"
#include "db/pipeline/field_path.h"
#include "util/mongoutils/str.h"

namespace mongo {

    using namespace mongoutils;

    const char FieldPath::prefix[] = "$";

    FieldPath::~FieldPath() {
    }

    FieldPath::FieldPath():
        vFieldName() {
    }

    FieldPath::FieldPath(const string &fieldPath):
        vFieldName() {
        /*
          The field path could be using dot notation.
          Break the field path up by peeling off successive pieces.
        */
        size_t startpos = 0;
        while(true) {
            /* find the next dot */
            const size_t dotpos = fieldPath.find('.', startpos);

            /* if there are no more dots, use the remainder of the string */
            if (dotpos == fieldPath.npos) {
                vFieldName.push_back(fieldPath.substr(startpos, dotpos));
                break;
            }

            /* use the string up to the dot */
            const size_t length = dotpos - startpos;
            uassert(15998, str::stream() <<
                    "field names cannot be zero length (in path \"" <<
                    fieldPath << "\")",
                    length > 0);

            vFieldName.push_back(fieldPath.substr(startpos, length));

            /* next time, search starting one spot after that */
            startpos = dotpos + 1;
        }
    }

    string FieldPath::getPath(bool fieldPrefix) const {
        stringstream ss;
        writePath(ss, fieldPrefix);
        return ss.str();
    }

    void FieldPath::writePath(ostream &outStream, bool fieldPrefix) const {
        if (fieldPrefix)
            outStream << prefix;

        outStream << vFieldName[0];

        const size_t n = vFieldName.size();
        for(size_t i = 1; i < n; ++i)
            outStream << "." << vFieldName[i];
    }

    FieldPath &FieldPath::operator=(const FieldPath &rRHS) {
        if (this != &rRHS) {
            vFieldName = rRHS.vFieldName;
        }

        return *this;
    }

}
