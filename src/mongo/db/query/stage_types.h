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

namespace mongo {

    /**
     * These map to implementations of the PlanStage interface, all of which live in db/exec/
     */
    enum StageType {
        STAGE_AND_HASH,
        STAGE_AND_SORTED,
        STAGE_COLLSCAN,
        STAGE_FETCH,
        STAGE_IXSCAN,
        STAGE_LIMIT,
        STAGE_OR,
        STAGE_SKIP,
        STAGE_SORT,
        STAGE_SORT_MERGE,
        STAGE_UNKNOWN,
    };

}  // namespace mongo
