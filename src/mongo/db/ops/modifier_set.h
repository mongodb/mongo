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
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/modifier_interface.h"

namespace mongo {

    class LogBuilder;

    class ModifierSet : public ModifierInterface {
        MONGO_DISALLOW_COPYING(ModifierSet);

    public:

        enum ModifierSetMode { SET_NORMAL, SET_ON_INSERT };
        explicit ModifierSet(ModifierSetMode mode = SET_NORMAL);

        //
        // Modifier interface implementation
        //

        virtual ~ModifierSet();

        /**
         * A 'modExpr' is a BSONElement {<fieldname>: <value>} coming from a $set mod such as
         * {$set: {<fieldname: <value>}}. init() extracts the field name and the value to be
         * assigned to it from 'modExpr'. It returns OK if successful or a status describing
         * the error.
         */
        virtual Status init(const BSONElement& modExpr, const Options& opts);

        /**
         * Looks up the field name in the sub-tree rooted at 'root', and binds, if necessary,
         * the '$' field part using the 'matchedfield' number. prepare() returns OK and
         * fills in 'execInfo' with information of whether this mod is a no-op on 'root' and
         * whether it is an in-place candidate. Otherwise, returns a status describing the
         * error.
         */
        virtual Status prepare(mutablebson::Element root,
                               const StringData& matchedField,
                               ExecInfo* execInfo);

        /**
         * Applies the prepared mod over the element 'root' specified in the prepare()
         * call. Returns OK if successful or a status describing the error.
         */
        virtual Status apply() const;

        /**
         * Adds a log entry to logRoot corresponding to the operation applied here. Returns OK
         * if successful or a status describing the error.
         */
        virtual Status log(LogBuilder* logBuilder) const;

    private:

        // Access to each component of fieldName that's the target of this mod.
        FieldRef _fieldRef;

        // 0 or index for $-positional in _fieldRef.
        size_t _posDollar;

        // If on 'on insert' mode, We'd like to apply this mod only if we're in a upsert.
        const ModifierSetMode _setMode;

        // Element of the $set expression.
        BSONElement _val;

        // See the class comments in modifier_interface.h
        ModifierInterface::Options _modOptions;

        // The instance of the field in the provided doc. This state is valid after a
        // prepare() was issued and until a log() is issued. The document this mod is
        // being prepared against must be live throughout all the calls.
        struct PreparedState;
        scoped_ptr<PreparedState> _preparedState;

    };

} // namespace mongo
