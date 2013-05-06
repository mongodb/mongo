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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * Abstract base class for update "modifiers" (a.k.a "$ operators"). To create a new
     * operator, implement a new derived class.
     *
     * A typical call sequence for the class is:
     *
     *   + init() with the mod arguments
     *
     *   + For each document that is being touched on that update, the following methods are
     *     going to be called once for that document and in the order the calls appear here.
     *
     *     + prepare() to check if mod is viable over the document
     *
     *     + apply(), effectively computing the update
     *
     *     + log() registering the change in the log for replication purposes
     *
     * Again, a modifier implementation may rely on these last three calls being made and in
     * that particular order and therefore can keep and reuse state between these calls, when
     * appropriate.
     *
     * TODO:
     * For a reference implementation, see modifier_identity.{h,cpp} used in tests.
     */
    class ModifierInterface {
    public:
        virtual ~ModifierInterface() { }

        /**
         * Returns OK and extracts the parameters for this given mod from 'modExpr'. For
         * instance, for a $inc, extracts the increment value. The init() method would be
         * called only once per operand, that is, if a { $inc: { a: 1, b: 1 } } is issued,
         * there would be one instance of the operator working on 'a' and one on 'b'. In each
         * case, init() would be called once with the respective bson element.
         *
         * If 'modExpr' is invalid, returns an error status with a reason description.
         *
         * Note:
         *
         *   + An operator may assume the modExpr passed here will be unchanged throughout all
         *     the mod object lifetime and also that the modExrp's lifetime exceeds the life
         *     time of this mod. Therefore, taking references to elements inside modExpr is
         *     valid.
         */
        virtual Status init(const BSONElement& modExpr) = 0;

        /**
         * Returns OK if it would be correct to apply this mod over the document 'root' (e.g, if
         * we're $inc-ing a field, is that field numeric in the current doc?).
         *
         * If the field this mod is targeted to contains a $-positional parameter, that value
         * can be bound with 'matchedField', passed by the caller.
         *
         * In addition, the call also identifies which fields(s) of 'root' the mod is interested
         * in changing (note that the modifier may want to add a field that's not present in
         * the document). The call also determines whether it could modify the document in
         * place and whether it is a no-op for the given document. All this information is in
         * the passed 'execInfo', which is filled inside the call.
         *
         * If the mod cannot be applied over 'root', returns an error status with a reason
         * description.
         */
        struct ExecInfo;
        virtual Status prepare(mutablebson::Element root,
                               const StringData& matchedField,
                               /* IN-OUT */ ExecInfo* execInfo) = 0;

        /**
         * Returns OK and modifies (or adds) an element (or elements) from the 'root' passed on
         * the prepareMod call. This may act on multiple fields but should only be called once
         * per operator.
         *
         * For this call to be issued, the call to 'prepareElem' must have necessarily turned
         * off 'ExecInfo.noOp', ie this mod over this document is not a no-op.
         *
         * If the mod could not be applied, returns an error status with a reason description.
         */
        virtual Status apply() const = 0 ;

        /**
         * Returns OK and registers the result of this mod in 'logRoot', the document that
         * would eventually become a log entry. The mod must have kept enough state to
         * be able to produce the log record (see idempotency note below). This call may be
         * issued even if apply() was not.
         *
         * If the mod could not be logged, returns an error status with a reason description.
         *
         * Idempotency Note:
         *
         *   + The modifier must log a mod that is idempotent, ie, applying it more than once
         *     to a base collection would produce the same result as applying it only once. For
         *     example, a $inc can be switched to a $set for the resulting incremented value,
         *     for logging purposes.  An array based operator may check the contents of the
         *     array before operating on it.
         */
        virtual Status log(mutablebson::Element logRoot) const = 0;

    };

    struct ModifierInterface::ExecInfo {
        static const int MAX_NUM_FIELDS = 2;

        ExecInfo() : inPlace(false), noOp(false) {
            for (int i = 0; i < MAX_NUM_FIELDS; i++) {
                fieldRef[i] = NULL;
            }
        }

        // The fields of concern to the driver: no other op may modify the fields listed here.
        FieldRef* fieldRef[MAX_NUM_FIELDS]; // not owned here
        bool inPlace;
        bool noOp;
    };

} // namespace mongo
