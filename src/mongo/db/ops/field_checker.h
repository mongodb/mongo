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

#pragma once

#include "mongo/base/status.h"

namespace mongo {

    class FieldRef;

    namespace fieldchecker {

        /**
         * Returns OK if all the below conditions on 'field' are valid:
         *   + Non-empty
         *   + Not the _id field (or a subfield of the _id field, such as _id.x.y)
         *   + Does not start or end with a '.'
         *   + Does not start with a $
         * Otherwise returns a code indicating cause of failure.
         */
        Status isUpdatable(const FieldRef& field);

        /**
         * Same behavior of isUpdatable but allowing update fields to start with '$'. This
         * supports $unset on legacy fields.
         */
        Status isUpdatableLegacy(const FieldRef& field);

        /**
         * Returns true, the position 'pos' of the first $-sign if present in 'fieldRef', and
         * how many other $-signs were found in 'count'. Otherwise return false.
         *
         * Note:
         *   isPositional assumes that the field is updatable. Call isUpdatable() above to
         *   verify.
         */
        bool isPositional(const FieldRef& fieldRef, size_t* pos, size_t* count = NULL);

    } // namespace fieldchecker

} // namespace mongo
