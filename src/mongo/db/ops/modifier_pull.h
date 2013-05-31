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

    class MatchExpression;

    class ModifierPull : public ModifierInterface {
        MONGO_DISALLOW_COPYING(ModifierPull);

    public:
        ModifierPull();
        virtual ~ModifierPull();

        /** Evaluates the array items to be removed and the match expression. */
        virtual Status init(const BSONElement& modExpr);

        /** Decides which portion of the array items will be removed from the provided element */
        virtual Status prepare(mutablebson::Element root,
                               const StringData& matchedField,
                               ExecInfo* execInfo);

        /** Updates the Element used in prepare with the effects of the $pull operation. */
        virtual Status apply() const;

        /** Converts the effects of this $pull into one or more equivalent $unset operations. */
        virtual Status log(mutablebson::Element logRoot) const;

    private:
        // Access to each component of fieldName that's the target of this mod.
        FieldRef _fieldRef;

        // 0 or index for $-positional in _fieldRef.
        size_t _posDollar;

        // A matcher built from the modExpr that we use to identify elements to remove.
        BSONObj _exprObj;
        scoped_ptr<MatchExpression> _matchExpression;

        struct PreparedState;
        scoped_ptr<PreparedState> _preparedState;
    };

} // namespace mongo
