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

        ModifierInc();
        virtual ~ModifierInc();

        /**
         * A 'modExpr' is a BSONElement {<fieldname>: <value>} coming from a $inc mod such as
         * {$inc: {<fieldname: <value>}}. init() extracts the field name and the value to be
         * assigned to it from 'modExpr'. It returns OK if successful or a status describing
         * the error.
         */
        virtual Status init(const BSONElement& modExpr);

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
