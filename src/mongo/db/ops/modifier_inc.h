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

#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/ops/modifier_interface.h"

namespace mongo {

    class LogBuilder;

    class ModifierInc : public ModifierInterface {
        MONGO_DISALLOW_COPYING(ModifierInc);

    public:

        // TODO: This is a shortcut to implementing $mul by hijacking $inc. In the near future,
        // we should consider either pulling $mul into its own operator, or creating a general
        // purpose "numeric binary op" operator. Potentially, that operator could also subsume
        // $bit (thought there are some subtleties, like that $bit can have multiple
        // operations, and doing so with arbirary math operations introduces potential
        // associativity difficulties). At the very least, if this mechanism is retained, then
        // this class should be renamed at some point away from ModifierInc.
        enum ModifierIncMode {
            MODE_INC,
            MODE_MUL
        };

        ModifierInc(ModifierIncMode mode = MODE_INC);
        virtual ~ModifierInc();

        /**
         * A 'modExpr' is a BSONElement {<fieldname>: <value>} coming from a $inc mod such as
         * {$inc: {<fieldname: <value>}}. init() extracts the field name and the value to be
         * assigned to it from 'modExpr'. It returns OK if successful or a status describing
         * the error.
         */
        virtual Status init(const BSONElement& modExpr, const Options& opts);

        /** Evaluates the validity of applying $inc to the identified node, and computes
         *  effects, handling upcasting and overflow as necessary.
         */
        virtual Status prepare(mutablebson::Element root,
                               const StringData& matchedField,
                               ExecInfo* execInfo);

        /** Updates the node passed in prepare with the results of the $inc */
        virtual Status apply() const;

        /** Converts the result of the $inc into an equivalent $set under logRoot */
        virtual Status log(LogBuilder* logBuilder) const;

    private:
        const ModifierIncMode _mode;

        // Access to each component of fieldName that's the target of this mod.
        FieldRef _fieldRef;

        // 0 or index for $-positional in _fieldRef.
        size_t _posDollar;

        // Element of the $set expression.
        SafeNum _val;

        struct PreparedState;
        scoped_ptr<PreparedState> _preparedState;
    };

} // namespace mongo
