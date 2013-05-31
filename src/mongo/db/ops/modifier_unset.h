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

    class ModifierUnset : public ModifierInterface {
        MONGO_DISALLOW_COPYING(ModifierUnset);

    public:

        ModifierUnset();

        //
        // Modifier interface implementation
        //

        virtual ~ModifierUnset();

        /**
         * A 'modExpr' is a BSONElement {<fieldname>: <value>} coming from a $set mod such as
         * {$unset: {<fieldname: <value>}}. init() extracts the field name and the value to be
         * assigned to it from 'modExpr'. It returns OK if successful or a status describing
         * the error.
         */
        virtual Status init(const BSONElement& modExpr);

        /**
         * Locates the field to be removed under the 'root' element, if it exist, and fills in
         * 'execInfo' accordingly. Return OK if successful or a status describing the error.
         */
        virtual Status prepare(mutablebson::Element root,
                               const StringData& matchedField,
                               ExecInfo* execInfo);

        /**
         * Removes the found element from the document. If such element was inside an array,
         * removal means setting that array position to 'null'.
         */
        virtual Status apply() const;

        /**
         * Adds the exact $unset mod to the log.
         */
        virtual Status log(mutablebson::Element logRoot) const;

    private:

        // Access to each component of fieldName that's the target of this mod.
        FieldRef _fieldRef;

        // 0 or index for $-positional in _fieldRef.
        size_t _posDollar;

        // Element of the $set expression.
        BSONElement _val;

        // The instance of the field in the provided doc. This state is valid after a
        // prepare() was issued and until a log() is issued. The document this mod is
        // being prepared against must be live throughout all the calls.
        struct PreparedState;
        scoped_ptr<PreparedState> _preparedState;

    };

} // namespace mongo
