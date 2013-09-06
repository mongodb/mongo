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

#include "mongo/db/field_ref_set.h"

#include "mongo/util/assert_util.h"

namespace mongo {

    namespace {

        // For legacy purposes, we must handle empty fieldnames, which FieldRef clearly
        // prohibits. It is preferrable to have FieldRef keep that constraint and relax it here
        // -- stricly in update code. The rationale is that, if we want to ban data with no
        // field names, we must allow that data to be updated.
        StringData safeFirstPart(const FieldRef* fieldRef) {
            if (fieldRef->numParts() == 0) {
                return StringData();
            }
            else {
                return fieldRef->getPart(0);
            }
        }

    }

    bool FieldRefSet::FieldRefPtrLessThan::operator()(const FieldRef* l, const FieldRef* r) const {
            return *l < *r;
    }

    FieldRefSet::FieldRefSet() {
    }

    bool FieldRefSet::insert(const FieldRef* toInsert, const FieldRef** conflict) {

        // We can determine if two fields conflict by checking their common prefix.
        //
        // If each field is exactly of the size of the common prefix, this means the fields are
        // the same. If one of the fields is greater than the common prefix and the other
        // isn't, the latter is a prefix of the former. And vice-versa.
        //
        // Example:
        //
        // inserted >      |    a          a.c
        // exiting  v      |   (0)        (+1)
        // ----------------|------------------------
        //      a (0)      |  equal      prefix <
        //      a.b (+1)   | prefix ^      *
        //
        // * Disjoint sub-trees

        // At each insertion, we only need to bother checking the fields in the set that have
        // at least some common prefix with the 'toInsert' field.
        StringData  prefixStr = safeFirstPart(toInsert);
        FieldRef prefixField;
        prefixField.parse(prefixStr);
        FieldSet::iterator it = _fieldSet.lower_bound(&prefixField);

        // Now, iterate over all the present fields in the set that have the same prefix.
        while (it != _fieldSet.end() && safeFirstPart(*it) == prefixStr) {
            size_t common = (*it)->commonPrefixSize(*toInsert);
            if ((*it)->numParts() == common || toInsert->numParts() == common) {
                *conflict = *it;
                return false;
            }
            ++it;
        }

        _fieldSet.insert(it, toInsert);
        *conflict = NULL;
        return true;
    }

} // namespace mongo
