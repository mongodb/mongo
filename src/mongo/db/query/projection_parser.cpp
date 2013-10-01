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

#include "mongo/db/query/projection_parser.h"

#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    // static
    Status ProjectionParser::parseFindSyntax(const BSONObj& obj, ParsedProjection** out) {
        auto_ptr<FindProjection> qp(new FindProjection());

        // Include _id by default.
        qp->_includeID = true;

        // By default include everything.
        bool lastNonIDValue = false;

        BSONObjIterator it(obj);
        while (it.more()) {
            BSONElement elt = it.next();
            if (mongoutils::str::equals("_id", elt.fieldName())) {
                qp->_includeID = elt.trueValue();
            }
            else {
                bool newFieldValue = elt.trueValue();
                if (qp->_excludedFields.size() + qp->_includedFields.size() > 0) {
                    // make sure we're all true or all false otherwise error
                    if (newFieldValue != lastNonIDValue) {
                        return Status(ErrorCodes::BadValue, "Inconsistent projection specs");
                    }
                }
                lastNonIDValue = newFieldValue;
                if (lastNonIDValue) {
                    // inclusive
                    qp->_includedFields.push_back(elt.fieldName());
                }
                else {
                    // exclusive
                    qp->_excludedFields.insert(elt.fieldName());
                }
            }
        }

        if (qp->_includedFields.size() > 0 && qp->_includeID) {
            qp->_includedFields.push_back(string("_id"));
        }

        *out = qp.release();
        return Status::OK();
    }

}  // namespace mongo
