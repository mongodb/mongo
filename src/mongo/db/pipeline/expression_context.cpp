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

#include "pch.h"

#include "db/curop.h"
#include "db/pipeline/expression_context.h"

namespace mongo {

    ExpressionContext::~ExpressionContext() {
    }

    inline ExpressionContext::ExpressionContext():
        inShard(false),
        inRouter(false),
        intCheckCounter(1) {
    }

    void ExpressionContext::checkForInterrupt() {
        /*
          Only really check periodically; the check gets a mutex, and could
          be expensive, at least in relative terms.
        */
#ifdef MONGO_LATER_SERVER_4844
        if ((++intCheckCounter % 128) == 0)
            killCurrentOp.checkForInterrupt();
#endif /* MONGO_LATER_SERVER_4844 */
    }

    ExpressionContext *ExpressionContext::create() {
        return new ExpressionContext();
    }

}
