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
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/db/interrupt_status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

    struct ExpressionContext : public IntrusiveCounterUnsigned {
    public:
        ExpressionContext(const InterruptStatus& status, const NamespaceString& ns)
            : inShard(false)
            , inRouter(false)
            , extSortAllowed(false)
            , interruptStatus(status)
            , ns(ns)
        {}
        /** Used by a pipeline to check for interrupts so that killOp() works.
         *  @throws if the operation has been interrupted
         */
        void checkForInterrupt() {
            // The check could be expensive, at least in relative terms.
            RARELY interruptStatus.checkForInterrupt();
        }
        
        bool inShard;
        bool inRouter;
        bool extSortAllowed;
        const InterruptStatus& interruptStatus;
        NamespaceString ns;
    };
}
