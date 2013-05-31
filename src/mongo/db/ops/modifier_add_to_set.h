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

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/ops/modifier_interface.h"

namespace mongo {

    class ModifierAddToSet : public ModifierInterface {
        MONGO_DISALLOW_COPYING(ModifierAddToSet);

    public:

        ModifierAddToSet();
        virtual ~ModifierAddToSet();

        /** Goes over the array item(s) that are going to be set- unioned and converts them
         *  internally to a mutable bson. Both single and $each forms are supported. Returns OK
         *  if the item(s) are valid otherwise returns a status describing the error.
         */
        virtual Status init(const BSONElement& modExpr);

        /** Decides which portion of the array items that are going to be set-unioned to root's
         *  document and fills in 'execInfo' accordingly. Returns OK if the document has a
         *  valid array to set-union to, othwise returns a status describing the error.
         */
        virtual Status prepare(mutablebson::Element root,
                               const StringData& matchedField,
                               ExecInfo* execInfo);

        /** Updates the Element used in prepare with the effects of the $addToSet operation. */
        virtual Status apply() const;

        /** Converts the effects of this $addToSet into one or more equivalent $set operations. */
        virtual Status log(mutablebson::Element logRoot) const;

    private:
        // Access to each component of fieldName that's the target of this mod.
        FieldRef _fieldRef;

        // 0 or index for $-positional in _fieldRef.
        size_t _posDollar;

        // Array of values to be set-union'ed onto target.
        mutablebson::Document _valDoc;
        mutablebson::Element _val;

        struct PreparedState;
        scoped_ptr<PreparedState> _preparedState;
    };

} // namespace mongo
