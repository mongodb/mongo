/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <stdlib.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/lockstate.h"


namespace mongo {

    class CurOp;
    class ProgressMeter;

    /**
     * This class encompasses the state required by an operation.
     *
     * TODO(HK): clarify what this means.  There's one OperationContext for one user operation...
     *           but is this true for getmore?  Also what about things like fsyncunlock / internal
     *           users / etc.?
     */
    class OperationContext  {
        MONGO_DISALLOW_COPYING(OperationContext);
    public:
        virtual ~OperationContext() { }

        /**
         * Interface for durability.  Caller DOES NOT own pointer.
         */
        virtual RecoveryUnit* recoveryUnit() const = 0;

        /**
         * Interface for locking.  Caller DOES NOT own pointer.
         */
        virtual LockState* lockState() const = 0;

        // --- operation level info? ---

        /**
         * TODO: Get rid of this and just have one interrupt func?
         * throws an exception if the operation is interrupted
         * @param heedMutex if true and have a write lock, won't kill op since it might be unsafe
         */
        virtual void checkForInterrupt(bool heedMutex = true) const = 0;

        /**
         * TODO: Where do I go
         * @return Status::OK() if not interrupted
         *         otherwise returns reasons
         */
        virtual Status checkForInterruptNoAssert() const = 0;

        /**
         * Delegates to CurOp, but is included here to break dependencies.
         * Caller does not own the pointer.
         */
        virtual ProgressMeter* setMessage(const char* msg,
                                          const std::string& name = "Progress",
                                          unsigned long long progressMeterTotal = 0,
                                          int secondsBetween = 3) = 0;

        /**
         * Delegates to CurOp, but is included here to break dependencies
         */
        virtual const char * getNS() const = 0;

        //
        // CurOp
        //

        /**
         * Returns CurOp. Caller does not own pointer
         */
        virtual CurOp* getCurOp() const = 0;

        /**
         * @return true if this instance is primary for this namespace
         */
        virtual bool isPrimaryFor( const StringData& ns ) = 0;

    protected:
        OperationContext() { }
    };

}  // namespace mongo
