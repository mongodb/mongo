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

#include "mongo/db/jsobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query_plan_selection_policy.h"

namespace mongo {

    class UpdateRequest {
    public:
        inline UpdateRequest(
            const NamespaceString& nsString,
            OpDebug& debug,
            const QueryPlanSelectionPolicy& policy = QueryPlanSelectionPolicy::any() )
            : _nsString(nsString)
            , _debug(debug)
            , _queryPlanPolicy(policy)
            , _god(false)
            , _upsert(false)
            , _multi(false)
            , _updateOpLog(false)
            , _fromMigration(false)
            , _fromReplication(false) {}

        const NamespaceString& getNamespaceString() const {
            return _nsString;
        }

        OpDebug& getDebug() {
            return _debug;
        }

        const OpDebug& getDebug() const {
            return _debug;
        }

        const QueryPlanSelectionPolicy& getQueryPlanSelectionPolicy() const {
            return _queryPlanPolicy;
        }

        inline void setQuery(const BSONObj& query) {
            _query = query;
        }

        inline const BSONObj& getQuery() const {
            return _query;
        }

        inline void setUpdates(const BSONObj& updates) {
            _updates = updates;
        }

        inline const BSONObj& getUpdates() const {
            return _updates;
        }

        // Please see documentation on the private members matching these names for
        // explanations of the following fields.

        inline void setGod(bool value = true) {
            _god = value;
        }

        bool isGod() const {
            return _god;
        }

        inline void setUpsert(bool value = true) {
            _upsert = value;
        }

        bool isUpsert() const {
            return _upsert;
        }

        inline void setMulti(bool value = true) {
            _multi = value;
        }

        bool isMulti() const {
            return _multi;
        }

        inline void setUpdateOpLog(bool value = true) {
            _updateOpLog = value;
        }

        bool shouldUpdateOpLog() const {
            return _updateOpLog;
        }

        inline void setFromMigration(bool value = true) {
            _fromMigration = value;
        }

        bool isFromMigration() const {
            return _fromMigration;
        }

        inline void setFromReplication(bool value = true) {
            _fromReplication = value;
        }

        bool isFromReplication() const {
            return _fromReplication;
        }

    private:

        const NamespaceString& _nsString;
        OpDebug& _debug;
        const QueryPlanSelectionPolicy& _queryPlanPolicy;

        // Contains the query that selects documents to update.
        BSONObj _query;

        // Contains the modifiers to apply to matched objects, or a replacement document.
        BSONObj _updates;

        // Flags controlling the update.

        // God bypasses _id checking and index generation. It is only used on behalf of system
        // updates, never user updates.
        bool _god;

        // True if this should insert if no matching document is found.
        bool _upsert;

        // True if this update is allowed to affect more than one document.
        bool _multi;

        // True if the effects of the update should be written to the oplog.
        bool _updateOpLog;

        // True if this update is on behalf of a chunk migration.
        bool _fromMigration;

        // True if this update is being applied during the application for the oplog.
        bool _fromReplication;
    };

} // namespace mongo
