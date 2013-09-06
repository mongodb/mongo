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

#include "mongo/db/query/query_projection.h"

#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    //
    // .find() syntax incl/excl projection
    // TODO: Test.
    //

    class InclExclProjection : public QueryProjection {
    public:
        virtual ~InclExclProjection() { }

        Status project(const WorkingSetMember& wsm, BSONObj* out) {
            BSONObjBuilder bob;
            if (_includeID) {
                BSONElement elt;
                if (!wsm.getFieldDotted("_id", &elt)) {
                    return Status(ErrorCodes::BadValue, "Couldn't get _id field in proj");
                }
                bob.append(elt);
            }

            if (_fieldsInclusive) {
                // We only want stuff in _fields.
                for (unordered_set<string>::const_iterator it = _fields.begin();
                     it != _fields.end(); ++it) {
                    BSONElement elt;
                    if (!wsm.getFieldDotted(*it, &elt)) {
                        return Status(ErrorCodes::BadValue, "no field " + *it + " in wsm to proj");
                    }
                    bob.append(elt);
                }
            }
            else {
                // We want stuff NOT in _fields.  This can't be covered, so we expect an obj.
                if (!wsm.hasObj()) {
                    return Status(ErrorCodes::BadValue,
                                  "exclusion specified for projection but no obj to iter over");
                }
                BSONObjIterator it(wsm.obj);
                while (it.more()) {
                    BSONElement elt = it.next();
                    if (_fields.end() == _fields.find(elt.fieldName())) {
                        bob.append(elt);
                    }
                }
            }

            *out = bob.obj();
            return Status::OK();
        }

    private:
        friend class QueryProjection;

        // _id can be included/excluded separately and is by default included.
        bool _includeID;

        // Either we include all of _fields or we exclude all of _fields.
        bool _fieldsInclusive;
        unordered_set<string> _fields;
    };

    // static
    Status QueryProjection::newInclusionExclusion(const BSONObj& obj, QueryProjection** out) {
        auto_ptr<InclExclProjection> qp(new InclExclProjection());

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
                if (qp->_fields.size() > 0) {
                    // make sure we're all true or all false otherwise error
                    if (newFieldValue != lastNonIDValue) {
                        return Status(ErrorCodes::BadValue, "Inconsistent projection specs");
                    }
                }
                lastNonIDValue = newFieldValue;
                qp->_fields.insert(elt.fieldName());
            }
        }

        qp->_fieldsInclusive = lastNonIDValue;
        *out = qp.release();
        return Status::OK();
    }

}  // namespace mongo
