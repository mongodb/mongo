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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"

namespace mongo {

class CollatorInterface;
class LogBuilder;

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
    virtual ~ModifierInterface() {}

    struct Options;
    /**
     * Returns OK and extracts the parameters for this given mod from 'modExpr'. For
     * instance, for a $inc, extracts the increment value. The init() method would be
     * called only once per operand, that is, if a { $inc: { a: 1, b: 1 } } is issued,
     * there would be one instance of the operator working on 'a' and one on 'b'. In each
     * case, init() would be called once with the respective bson element.
     *
     * If 'modExpr' is invalid, returns an error status with a reason description.
     *
     * The optional bool out parameter 'positional', if provided, will be set to 'true' if
     * the mod requires matched field details to be provided when calling 'prepare'. The
     * field is optional since this is a hint to the caller about what work is needed to
     * correctly invoke 'prepare'. It is always legal to provide any match details
     * unconditionally. The value set in 'positional' if any, is only meaningful if 'init'
     * returns an OK status.
     *
     * Note:
     *
     *   + An operator may assume the modExpr passed here will be unchanged throughout all
     *     the mod object lifetime and also that the modExrp's lifetime exceeds the life
     *     time of this mod. Therefore, taking references to elements inside modExpr is
     *     valid.
     */
    virtual Status init(const BSONElement& modExpr,
                        const Options& opts,
                        bool* positional = NULL) = 0;

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
     *
     * Note that you must provide a meaningful 'matchedField' here, unless 'init' set
     * 'positional' to 'false', in which case you may pass an empty StringData object.
     */
    struct ExecInfo;
    virtual Status prepare(mutablebson::Element root,
                           StringData matchedField,
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
    virtual Status apply() const = 0;

    /**
     * Returns OK and records the result of this mod in the provided LogBuilder. The mod
     * must have kept enough state to be able to produce the log record (see idempotency
     * note below). This call may be issued even if apply() was not.
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
    virtual Status log(LogBuilder* logBuilder) const = 0;

    /**
     * Set the collation on the modifier.  This is a no-op on modifiers that are not
     * collation-aware.
     *
     * setCollator() should update any initialization that occured during init() to respect the
     * provided collator, which may be different than the collator provided in the modifier
     * options.
     *
     * If setCollator() is called, it is required that the current collator of the modifier is
     * the simple collator (nullptr).
     *
     * The collator must outlive the modifier interface.
     */
    virtual void setCollator(const CollatorInterface* collator) = 0;
};

/**
 * Options used to control Modifier behavior
 */
struct ModifierInterface::Options {
    Options() = default;
    Options(bool repl, bool ofs, const CollatorInterface* collator)
        : fromReplication(repl), enforceOkForStorage(ofs), collator(collator) {}

    static Options normal(const CollatorInterface* collator = nullptr) {
        return Options(false, true, collator);
    }
    static Options fromRepl(const CollatorInterface* collator = nullptr) {
        return Options(true, false, collator);
    }

    bool fromReplication = false;
    bool enforceOkForStorage = true;
    const CollatorInterface* collator = nullptr;
};

struct ModifierInterface::ExecInfo {
    static const int MAX_NUM_FIELDS = 2;

    /**
     * An update mod may specify that it wishes to the applied only if the context
     * of the update turns out a certain way.
     */
    enum UpdateContext {
        // This mod wants to be applied only if the update turns out to be an insert.
        INSERT_CONTEXT,

        // This mod wants to be applied only if the update is not an insert.
        UPDATE_CONTEXT,

        // This mod doesn't care if the update will be an update or an upsert.
        ANY_CONTEXT
    };

    ExecInfo() : noOp(false), context(ANY_CONTEXT) {
        for (int i = 0; i < MAX_NUM_FIELDS; i++) {
            fieldRef[i] = NULL;
        }
    }

    // The fields of concern to the driver: no other op may modify the fields listed here.
    FieldRef* fieldRef[MAX_NUM_FIELDS];  // not owned here
    bool noOp;
    UpdateContext context;
};

}  // namespace mongo
