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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/index_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/modifier_interface.h"

namespace mongo {

    class UpdateDriver {
    public:

        struct Options;
        UpdateDriver(const Options& opts);

        ~UpdateDriver();

        /**
         * Returns OK and fills in '_mods' if 'updateExpr' is correct. In that case, take note
         * that the collection that 'updateExpr' is built against have indices that encompass
         * all fields present in 'indexedFields'. Otherwise returns an error status with a
         * corresponding description.
         */
        Status parse(const IndexPathSet& indexedFields, const BSONObj& updateExpr);

        /**
         * Returns true and derives a BSONObj, 'newObj', from 'query'.
         *
         * TODO: Elaborate on the logic used here.
         */
        bool createFromQuery(const BSONObj query, BSONObj* newObj) const;

        /**
         * Returns OK and executes '_mods' over 'doc', generating 'newObj'. If any mod is
         * positional, use 'matchedField' (index of the array item matched). If doc allows
         * mods to be applied in place and no index updating is involved, then the mods may
         * be applied "in place" over 'doc'.
         *
         * If the driver's '_logOp' mode is turned on, and if 'logOpRec' is not NULL, fills in
         * the latter with the oplog entry corresponding to the update. If '_mods' can't be
         * applied, returns an error status with a corresponding description.
         */
        Status update(const StringData& matchedField,
                      mutablebson::Document* doc,
                      BSONObj* logOpRec);

        //
        // Accessors
        //

        size_t numMods() const;

        bool dollarModMode() const;

        bool modsAffectIndices() const;

        bool multi() const;
        void setMulti(bool multi);

        bool upsert() const;
        void setUpsert(bool upsert);

        bool logOp() const;
        void setLogOp(bool logOp);

        ModifierInterface::ExecInfo::UpdateContext context() const;
        void setContext(ModifierInterface::ExecInfo::UpdateContext context);

    private:

        /** Resets the state of the class associated with mods (not the error state) */
        void clear();

        //
        // immutable properties after parsing
        //

        // Is there a list of $mod's on '_mods' or is it just full object replacment?
        bool _dollarModMode;

        // Collection of update mod instances. Owned here.
        vector<ModifierInterface*> _mods;

        // What are the list of fields in the collection over which the update is going to be
        // applied that participate in indices?
        IndexPathSet _indexedFields;

        //
        // mutable properties after parsing
        //

        // May this driver apply updates to several documents?
        bool _multi;

        // May this driver construct a new object if an update for a non-existing one is sent?
        bool _upsert;

        // Should this driver generate an oplog record when it applies the update?
        bool _logOp;

        // Are any of the fields mentioned in the mods participating in any index? Is set anew
        // at each call to update.
        bool _affectIndices;

        // Is this update going to be an upsert?
        ModifierInterface::ExecInfo::UpdateContext _context;
    };

    struct UpdateDriver::Options {
        bool multi;
        bool upsert;
        bool logOp;

        Options() : multi(false), upsert(false), logOp(false) {}
    };

} // namespace mongo
