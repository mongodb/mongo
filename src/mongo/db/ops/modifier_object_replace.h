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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/modifier_interface.h"

namespace mongo {

    class LogBuilder;

    class ModifierObjectReplace : public ModifierInterface {
        MONGO_DISALLOW_COPYING(ModifierObjectReplace);

    public:

        ModifierObjectReplace();

        //
        // Modifier interface implementation
        //

        virtual ~ModifierObjectReplace();


        /**
         * Returns true and takes the embedded object contained in 'modExpr' to be the object
         * we're replacing for. The field name of 'modExpr' is ignored. If 'modExpr' is in an
         * unexpected format or if it can't be parsed for some reason, returns an error status
         * describing the error.
         */
        virtual Status init(const BSONElement& modExpr);

        /**
         * Registers the that 'root' is in the document that we want to fully replace.
         * prepare() returns OK and always fills 'execInfo' with true for
         * noOp.
         */
        virtual Status prepare(mutablebson::Element root,
                               const StringData& matchedField,
                               ExecInfo* execInfo);

        /**
         * Replaces the document passed in prepare() for the object passed in init().  Returns
         * OK if successful or a status describing the error.
         */
        virtual Status apply() const;

        /**
         * Adds a log entry to logRoot corresponding to full object replacement. Returns OK if
         * successful or a status describing the error.
         */
        virtual Status log(LogBuilder* logBuilder) const;

    private:

        // Object to replace with.
        BSONObj _val;

        // The document whose value needs to be replaced. This state is valid after a prepare()
        // was issued and until a log() is issued. The document this mod is being prepared
        // against must e live throughout all the calls.
        struct PreparedState;
        scoped_ptr<PreparedState> _preparedState;
    };

} // namespace mongo
