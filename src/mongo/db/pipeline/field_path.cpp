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

#include "mongo/pch.h"

#include "mongo/db/pipeline/field_path.h"

#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using namespace mongoutils;

    const char FieldPath::prefix[] = "$";

    FieldPath::FieldPath(const vector<string>& fieldPath) {
        massert(16409, "FieldPath cannot be constructed from an empty vector.", !fieldPath.empty());
        vFieldName.reserve(fieldPath.size());
        for(vector<string>::const_iterator i = fieldPath.begin(); i != fieldPath.end(); ++i) {
            pushFieldName(*i);
        }
        verify(getPathLength() > 0);
    }

    FieldPath::FieldPath(const string& fieldPath) {
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
                string lastFieldName = fieldPath.substr(startpos, dotpos);
                pushFieldName(lastFieldName);
                break;
            }

            /* use the string up to the dot */
            const size_t length = dotpos - startpos;
            string nextFieldName = fieldPath.substr(startpos, length);
            pushFieldName(nextFieldName);

            /* next time, search starting one spot after that */
            startpos = dotpos + 1;
        }
        verify(getPathLength() > 0);
    }

    string FieldPath::getPath(bool fieldPrefix) const {
        stringstream ss;
        writePath(ss, fieldPrefix);
        return ss.str();
    }

    void FieldPath::writePath(ostream &outStream, bool fieldPrefix) const {
        if (fieldPrefix)
            outStream << prefix;

        const size_t n = vFieldName.size();

        verify(n > 0);
        outStream << vFieldName[0];
        for(size_t i = 1; i < n; ++i)
            outStream << '.' << vFieldName[i];
    }

    FieldPath FieldPath::tail() const {
        vector<string> allButFirst(vFieldName.begin()+1, vFieldName.end());
        return FieldPath(allButFirst);
    }

    void FieldPath::uassertValidFieldName(const string& fieldName) {
        uassert(15998, "FieldPath field names may not be empty strings.", fieldName.length() > 0);
        uassert(16410, "FieldPath field names may not start with '$'.", fieldName[0] != '$');
        uassert(16411, "FieldPath field names may not contain '\0'.",
                fieldName.find('\0') == string::npos);
        uassert(16412, "FieldPath field names may not contain '.'.",
                !str::contains(fieldName, '.'));
    }

    void FieldPath::pushFieldName(const string& fieldName) {
        uassertValidFieldName(fieldName);
        vFieldName.push_back(fieldName);
    }

}
