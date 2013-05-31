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
#include "mongo/util/safe_num.h"

namespace mongo {

    class ModifierBit : public ModifierInterface {
        MONGO_DISALLOW_COPYING(ModifierBit);

    public:

        ModifierBit();
        virtual ~ModifierBit();

        /**
         * A 'modExpr' is a BSONElement {<fieldname>: <value>} coming from a $bit mod such as
         * {$bit: {<field: { [and|or] : <value>}}. init() extracts the field name, the
         * operation subtype, and the value to be assigned to it from 'modExpr'. It returns OK
         * if successful or a status describing the error.
         */
        virtual Status init(const BSONElement& modExpr);

        /** Validates the potential application of the init'ed mod to the given Element and
         *  configures the internal state of the mod as necessary.
         */
        virtual Status prepare(mutablebson::Element root,
                               const StringData& matchedField,
                               ExecInfo* execInfo);

        /** Updates the Element used in prepare with the effects of the $bit operation */
        virtual Status apply() const;

        /** Converts the effects of this $bit into an equivalent $set */
        virtual Status log(mutablebson::Element logRoot) const;

    private:
        // Access to each component of fieldName that's the target of this mod.
        FieldRef _fieldRef;

        // 0 or index for $-positional in _fieldRef.
        size_t _posDollar;

        // Value to be $bit'ed onto target
        SafeNum _val;

        // The operator on SafeNum that we will invoke.
        SafeNum (SafeNum::* _op)(const SafeNum&) const;

        struct PreparedState;
        scoped_ptr<PreparedState> _preparedState;
    };

} // namespace mongo
