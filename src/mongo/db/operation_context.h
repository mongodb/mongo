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
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {

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

        // XXX: migrate callers use the recoveryUnit() directly
        template <typename T>
        T* writing(T* x) {
            return recoveryUnit()->writing(x);
        }

        int& writingInt(int& d) {
            return recoveryUnit()->writingInt(d);
        }

        void syncDataAndTruncateJournal() {
            recoveryUnit()->syncDataAndTruncateJournal();
        }

        void createdFile(const std::string& filename, unsigned long long len) {
            recoveryUnit()->createdFile(filename, len);
        }

        void* writingPtr(void* data, size_t len) {
            return recoveryUnit()->writingPtr(data, len);
        }

        bool isCommitNeeded() const {
            return recoveryUnit()->isCommitNeeded();
        }

        bool commitIfNeeded(bool force = false) {
            return recoveryUnit()->commitIfNeeded(force);
        }
        // XXX: migrate callers use the recoveryUnit() directly


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
         * TODO(ERH): this should move to some CurOp like context.
         */
        virtual ProgressMeter* setMessage(const char* msg,
                                          const std::string& name = "Progress",
                                          unsigned long long progressMeterTotal = 0,
                                          int secondsBetween = 3) = 0;

        /**
         * Returns a OperationContext. Caller takes ownership.
         *
         * This interface is used for functions that need to create transactions (aka OpCtx), but
         * don't know which implementation they should create. It allows the calling code to make
         * that decision for them.
         *
         * TODO come up with a better Factory API once we split this class up (SERVER-13931).
         */
        typedef OperationContext* (*Factory)();

        /**
         * A OperationContext::Factory that always returns NULL. For things that shouldn't be
         * touching their txns such as mongos or some unittests.
         */
        static OperationContext* factoryNULL() { return NULL; }

    protected:
        OperationContext() { }
    };

}  // namespace mongo
