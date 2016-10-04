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

#include <string>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/modifier_interface.h"
#include "mongo/db/ops/modifier_table.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/update_index_data.h"

namespace mongo {

class CollatorInterface;
class OperationContext;

class UpdateDriver {
public:
    struct Options;
    UpdateDriver(const Options& opts);

    ~UpdateDriver();

    /**
     * Returns OK and fills in '_mods' if 'updateExpr' is correct. Otherwise returns an
     * error status with a corresponding description.
     */
    Status parse(const BSONObj& updateExpr, const bool multi = false);

    /**
     * Fills in document with any fields in the query which are valid.
     *
     * Valid fields include equality matches like "a":1, or "a.b":false
     *
     * Each valid field will be expanded (from dot notation) and conflicts will be
     * checked for all fields added to the underlying document.
     *
     * Returns Status::OK() if the document can be used. If there are any error or
     * conflicts along the way then those errors will be returned.
     */
    Status populateDocumentWithQueryFields(OperationContext* txn,
                                           const BSONObj& query,
                                           const std::vector<FieldRef*>* immutablePaths,
                                           mutablebson::Document& doc) const;

    Status populateDocumentWithQueryFields(const CanonicalQuery& query,
                                           const std::vector<FieldRef*>* immutablePaths,
                                           mutablebson::Document& doc) const;

    /**
     * return a BSONObj with the _id field of the doc passed in, or the doc itself.
     * If no _id and multi, error.
     */
    BSONObj makeOplogEntryQuery(const BSONObj& doc, bool multi) const;

    /**
     * Returns OK and executes '_mods' over 'doc', generating 'newObj'. If any mod is
     * positional, use 'matchedField' (index of the array item matched). If doc allows
     * mods to be applied in place and no index updating is involved, then the mods may
     * be applied "in place" over 'doc'.
     *
     * If the driver's '_logOp' mode is turned on, and if 'logOpRec' is not NULL, fills in
     * the latter with the oplog entry corresponding to the update. If '_mods' can't be
     * applied, returns an error status with a corresponding description.
     *
     * If a non-NULL updatedField vector* is supplied,
     * then all updated fields will be added to it.
     */
    Status update(StringData matchedField,
                  mutablebson::Document* doc,
                  BSONObj* logOpRec = NULL,
                  FieldRefSet* updatedFields = NULL,
                  bool* docWasModified = NULL);

    //
    // Accessors
    //

    size_t numMods() const;

    bool isDocReplacement() const;

    bool modsAffectIndices() const;
    void refreshIndexKeys(const UpdateIndexData* indexedFields);

    bool logOp() const;
    void setLogOp(bool logOp);

    ModifierInterface::Options modOptions() const;
    void setModOptions(ModifierInterface::Options modOpts);

    ModifierInterface::ExecInfo::UpdateContext context() const;
    void setContext(ModifierInterface::ExecInfo::UpdateContext context);

    mutablebson::Document& getDocument() {
        return _objDoc;
    }

    const mutablebson::Document& getDocument() const {
        return _objDoc;
    }

    bool needMatchDetails() const {
        return _positional;
    }

    /**
     * Set the collator which will be used by all of the UpdateDriver's underlying modifiers.
     *
     * 'collator' must outlive the UpdateDriver.
     */
    void setCollator(const CollatorInterface* collator);

private:
    /** Resets the state of the class associated with mods (not the error state) */
    void clear();

    /** Create the modifier and add it to the back of the modifiers vector */
    inline Status addAndParse(const modifiertable::ModifierType type, const BSONElement& elem);

    //
    // immutable properties after parsing
    //

    // Is there a list of $mod's on '_mods' or is it just full object replacement?
    bool _replacementMode;

    // Collection of update mod instances. Owned here.
    std::vector<ModifierInterface*> _mods;

    // What are the list of fields in the collection over which the update is going to be
    // applied that participate in indices?
    //
    // NOTE: Owned by the collection's info cache!.
    const UpdateIndexData* _indexedFields;

    //
    // mutable properties after parsing
    //

    // Should this driver generate an oplog record when it applies the update?
    bool _logOp;

    // The options to initiate the mods with
    ModifierInterface::Options _modOptions;

    // Are any of the fields mentioned in the mods participating in any index? Is set anew
    // at each call to update.
    bool _affectIndices;

    // Do any of the mods require positional match details when calling 'prepare'?
    bool _positional;

    // Is this update going to be an upsert?
    ModifierInterface::ExecInfo::UpdateContext _context;

    // The document used to represent or store the object being updated.
    mutablebson::Document _objDoc;

    // The document used to build the oplog entry for the update.
    mutablebson::Document _logDoc;
};

struct UpdateDriver::Options {
    bool logOp;
    ModifierInterface::Options modOptions;

    Options() : logOp(false), modOptions() {}
};

}  // namespace mongo
