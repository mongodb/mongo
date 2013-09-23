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

#include "mongo/db/exec/projection_executor.h"

#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/parsed_projection.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    // static
    Status ProjectionExecutor::apply(const ParsedProjection* proj, WorkingSetMember* wsm) {
        if (ParsedProjection::FIND_SYNTAX == proj->getType()) {
            return applyFindSyntax(static_cast<const FindProjection*>(proj), wsm);
        }
        else {
            return Status(ErrorCodes::BadValue, "trying to apply unknown projection type");
        }
    }

    // static
    Status ProjectionExecutor::applyFindSyntax(const FindProjection* proj, WorkingSetMember* wsm) {
        BSONObjBuilder bob;
        if (proj->_includeID) {
            BSONElement elt;
            if (!wsm->getFieldDotted("_id", &elt)) {
                return Status(ErrorCodes::BadValue, "Couldn't get _id field in proj");
            }
            bob.append(elt);
        }

        if (proj->_includedFields.size() > 0) {
            // We only want stuff in _fields.
            const vector<string>& fields = proj->_includedFields;
            for (size_t i = 0; i < fields.size(); ++i) {
                BSONElement elt;
                // We can project a field that doesn't exist.  We just ignore it.
                // UNITTEST 11738048
                if (wsm->getFieldDotted(fields[i], &elt) && !elt.eoo()) {
                    bob.append(elt);
                }
            }
        }
        else {
            // We want stuff NOT in _fields.  This can't be covered, so we expect an obj.
            if (!wsm->hasObj()) {
                return Status(ErrorCodes::BadValue,
                        "exclusion specified for projection but no obj to iter over");
            }
            const unordered_set<string>& fields = proj->_excludedFields;
            BSONObjIterator it(wsm->obj);
            while (it.more()) {
                BSONElement elt = it.next();
                if (!mongoutils::str::equals("_id", elt.fieldName())) {
                    if (fields.end() == fields.find(elt.fieldName())) {
                        bob.append(elt);
                    }
                }
            }
        }

        wsm->state = WorkingSetMember::OWNED_OBJ;
        wsm->obj = bob.obj();
        wsm->keyData.clear();
        wsm->loc = DiskLoc();
        return Status::OK();
    }

}  // namespace mongo
