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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/ops/path_support.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace pathsupport {

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

        Status maybePadTo(mutablebson::Element* elemArray,
                          size_t sizeRequired) {
            dassert(elemArray->getType() == Array);

            if (sizeRequired > kMaxPaddingAllowed) {
                return Status(ErrorCodes::CannotBackfillArray,
                              mongoutils::str::stream() << "can't backfill array to larger than "
                                                        << kMaxPaddingAllowed << " elements");
            }

            size_t currSize = mutablebson::countChildren(*elemArray);
            if (sizeRequired > currSize) {
                size_t toPad = sizeRequired - currSize;
                for (size_t i = 0; i < toPad; i++) {
                    Status status = elemArray->appendNull("");
                    if (!status.isOK()) {
                        return status;
                    }
                }
            }
            return Status::OK();
        }

    } // unnamed namespace

    Status findLongestPrefix(const FieldRef& prefix,
                             mutablebson::Element root,
                             size_t* idxFound,
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
                          mongoutils::str::stream() << "cannot use the part (" <<
                          prefix.getPart(i-1) << " of " << prefix.dottedField() <<
                          ") to traverse the element ({" <<
                          curr.toString() << "})");
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

    Status createPathAt(const FieldRef& prefix,
                        size_t idxFound,
                        mutablebson::Element elemFound,
                        mutablebson::Element newElem) {
        Status status = Status::OK();

        // Sanity check that 'idxField' is an actual part.
        const size_t size = prefix.numParts();
        if (idxFound >= size) {
            return Status(ErrorCodes::BadValue, "index larger than path size");
        }

        mutablebson::Document& doc = elemFound.getDocument();

        // If we are creating children under an array and a numeric index is next, then perhaps
        // we need padding.
        size_t i = idxFound;
        bool inArray = false;
        if (elemFound.getType() == mongo::Array) {
            size_t newIdx = 0;
            if (!isNumeric(prefix.getPart(idxFound), &newIdx)) {
                return Status(ErrorCodes::InvalidPath, "Array require numeric fields");
            }

            status = maybePadTo(&elemFound, newIdx);
            if (!status.isOK()) {
                return status;
            }

            // If there is a next field, that would be an array element. We'd like to mark that
            // field because we create array elements differently than we do regular objects.
            if (++i < size) {
                inArray = true;
            }
        }

        // Create all the remaining parts but the last one.
        for (; i < size - 1 ; i++) {
            mutablebson::Element elem = doc.makeElementObject(prefix.getPart(i));
            if (!elem.ok()) {
                return Status(ErrorCodes::InternalError, "cannot create path");
            }

            // If this field is an array element, we wrap it in an object (because array
            // elements are wraped in { "N": <element> } objects.
            if (inArray) {
                // TODO pass empty StringData to makeElementObject, when that's supported.
                mutablebson::Element arrayObj = doc.makeElementObject("" /* it's an array */);
                if (!arrayObj.ok()) {
                    return Status(ErrorCodes::InternalError, "cannot create item on array");
                }
                status = arrayObj.pushBack(elem);
                if (!status.isOK()) {
                    return status;
                }
                status = elemFound.pushBack(arrayObj);
                if (!status.isOK()) {
                    return status;
                }
                inArray = false;
            }
            else {
                status = elemFound.pushBack(elem);
                if (!status.isOK()) {
                    return status;
                }
            }

            elemFound = elem;
        }

        // Attach the last element. Here again, if we're in a field that is an array element,
        // we wrap it in an object first.
        if (inArray) {
            // TODO pass empty StringData to makeElementObject, when that's supported.
            mutablebson::Element arrayObj = doc.makeElementObject("" /* it's an array */);
            if (!arrayObj.ok()) {
                return Status(ErrorCodes::InternalError, "cannot create item on array");
            }

            status = arrayObj.pushBack(newElem);
            if (!status.isOK()) {
                return status;
            }

            status = elemFound.pushBack(arrayObj);
            if (!status.isOK()) {
                return status;
            }

        }
        else {
            status = elemFound.pushBack(newElem);
            if (!status.isOK()) {
                return status;
            }
        }

        return Status::OK();
    }

} // namespace pathsupport
} // namespace mongo
