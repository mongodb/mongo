/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/pch.h"

#include "mongo/db/pipeline/accumulator.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
    using namespace mongoutils;

    void agg_framework_reservedErrors() {
        uassert(16030, "reserved error", false);
        uassert(16031, "reserved error", false);
        uassert(16032, "reserved error", false);
        uassert(16033, "reserved error", false);

        uassert(16036, "reserved error", false);
        uassert(16037, "reserved error", false);
        uassert(16038, "reserved error", false);
        uassert(16039, "reserved error", false);
        uassert(16040, "reserved error", false);
        uassert(16041, "reserved error", false);
        uassert(16042, "reserved error", false);
        uassert(16043, "reserved error", false);
        uassert(16044, "reserved error", false);
        uassert(16045, "reserved error", false);
        uassert(16046, "reserved error", false);
        uassert(16047, "reserved error", false);
        uassert(16048, "reserved error", false);
        uassert(16049, "reserved error", false);
    }
}
