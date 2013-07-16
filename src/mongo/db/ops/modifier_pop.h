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
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/modifier_interface.h"

namespace mongo {

    class LogBuilder;

    class ModifierPop : public ModifierInterface {
        MONGO_DISALLOW_COPYING(ModifierPop);

    public:

        ModifierPop();
        virtual ~ModifierPop();

        /**
         * The format of this modifier ($pop) is {<fieldname>: <value>}.
         * If the value is number and greater than -1 then an element is removed from the bottom,
         * otherwise the top. Currently the value can be any anything but we document
         * the use of the numbers "1, -1" only.
         *
         * Ex. $pop: {'a':1} will remove the last item from this array: [1,2,3] -> [1,2]
         */
        virtual Status init(const BSONElement& modExpr);

        virtual Status prepare(mutablebson::Element root,
                               const StringData& matchedField,
                               ExecInfo* execInfo);


        virtual Status apply() const;

        virtual Status log(LogBuilder* logBuilder) const;

    private:

        // Access to each component of fieldName that's the target of this mod.
        FieldRef _fieldRef;

        // 0 or index for $-positional in _fieldRef.
        size_t _positionalPathIndex;

        // element position to remove from
        bool _fromTop;

        // The instance of the field in the provided doc.
        // This data is valid after prepare, for use by log and apply
        struct PreparedState;
        scoped_ptr<PreparedState> _preparedState;
    };

} // namespace mongo
