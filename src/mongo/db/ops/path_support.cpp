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

#include "mongo/db/ops/path_support.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/element.h"

namespace mongo {

    namespace {

        bool isNumeric(const StringData& str, size_t* num) {
            size_t res = 0;
            for (size_t i = 0; i < str.size(); ++i) {
                if (str[i] < '0' || str[i] > '9') {
                    return false;
                }
                else {
                    res = res * 10 + (str[i] - '0');
                }
            }
            *num = res;
            return true;
        }

    } // unnamed namespace

    Status PathSupport::findLongestPrefix(const FieldRef& prefix,
                                          mutablebson::Element root,
                                          int32_t* idxFound,
                                          mutablebson::Element* elemFound) {

        // If root is empty or the prefix is so, there's no point in looking for a prefix.
        const size_t prefixSize = prefix.numParts();
        if (!root.hasChildren() || prefixSize == 0) {
            return Status(ErrorCodes::NonExistentPath,
                          "either the document or the path are empty");
        }

        // Loop through prefix's parts. At each iteration, check that the part ('curr') exists
        // in 'root' and that the type of the previous part ('prev') allows for children.
        mutablebson::Element curr = root;
        mutablebson::Element prev = root;
        size_t i = 0;
        size_t numericPart = 0;
        bool viable = true;
        for (;i < prefixSize; i++) {

            // If prefix wants to reach 'curr' by applying a non-numeric index to an array
            // 'prev', or if 'curr' wants to traverse a leaf 'prev', then we'd be in a
            // non-viable path (see definition on the header file).
            StringData prefixPart = prefix.getPart(i);
            prev = curr;
            switch (curr.getType()) {

            case Object:
                curr = prev[prefixPart];
                break;

            case Array:
                if (!isNumeric(prefixPart, &numericPart)) {
                    viable = false;
                } else {
                    curr = prev[numericPart];
                }
                break;

            default:
                viable = false;
            }

            // If we couldn't find the next field part of the prefix in the document or if the
            // field part we're in constitutes a non-viable path, we can stop looking.
            if (!curr.ok() || !viable) {
                break;
            }
        }

        // We broke out of the loop because one of four things happened. (a) 'prefix' and
        // 'root' have nothing in common, (b) 'prefix' is not viable in 'root', (c) not all the
        // parts in 'prefix' exist in 'root', or (d) all parts do. In each case, we need to
        // figure out what index and Element pointer to return.
        if (i == 0) {
            return Status(ErrorCodes::NonExistentPath,
                          "cannot find path in the document");
        }
        else if (!viable) {
            *idxFound = i - 1;
            *elemFound = prev;
            return Status(ErrorCodes::PathNotViable,
                          "cannot use the part to traverse the document");
        }
        else if (curr.ok()) {
            *idxFound = i - 1;
            *elemFound = curr;
            return Status::OK();
        }
        else {
            *idxFound = i - 1;
            *elemFound = prev;
            return Status::OK();
        }
    }

} // namespace mongo
