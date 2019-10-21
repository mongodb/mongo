/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * This file was originally copied out of the firefox 38.0.1esr source tree from
 * `js/src/vm/PosixNSPR.cpp` and modified to use the MongoDB threading primitives.
 *
 * The point of this file is to provide dummy implementations such that when the SpiderMonkey build
 * looks for these symbols it will find symbols, thus permitting linkage.  No code uses these
 * entrypoints.
 */

#include "mongo/platform/basic.h"

#include <array>
#include <js/Utility.h>
#include <vm/PosixNSPR.h>

#include "mongo/platform/mutex.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/time_support.h"

#define MONGO_MOZ_UNIMPLEMENTED(ReturnType, funcName, ...) \
    ReturnType funcName(__VA_ARGS__) {                     \
        MOZ_CRASH(#funcName " unimplemented");             \
    }

MONGO_MOZ_UNIMPLEMENTED(void, mongo::mozjs::PR_BindThread, PRThread*);
MONGO_MOZ_UNIMPLEMENTED(PRThread*, mongo::mozjs::PR_CreateFakeThread);
MONGO_MOZ_UNIMPLEMENTED(void, mongo::mozjs::PR_DestroyFakeThread, PRThread*);

MONGO_MOZ_UNIMPLEMENTED(PRThread*,
                        PR_CreateThread,
                        PRThreadType,
                        void (*)(void*),
                        void*,
                        PRThreadPriority,
                        PRThreadScope,
                        PRThreadState,
                        uint32_t);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_JoinThread, PRThread*);

MONGO_MOZ_UNIMPLEMENTED(PRThread*, PR_GetCurrentThread);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_SetCurrentThreadName, const char*);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_NewThreadPrivateIndex, unsigned*, PRThreadPrivateDTOR);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_SetThreadPrivate, unsigned, void*);

MONGO_MOZ_UNIMPLEMENTED(void*, PR_GetThreadPrivate, unsigned);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_CallOnce, PRCallOnceType*, PRCallOnceFN);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_CallOnceWithArg, PRCallOnceType*, PRCallOnceWithArgFN, void*);

MONGO_MOZ_UNIMPLEMENTED(PRLock*, PR_NewLock);

MONGO_MOZ_UNIMPLEMENTED(void, PR_DestroyLock, PRLock*);

MONGO_MOZ_UNIMPLEMENTED(void, PR_Lock, PRLock*);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_Unlock, PRLock*);

MONGO_MOZ_UNIMPLEMENTED(PRCondVar*, PR_NewCondVar, PRLock*);

MONGO_MOZ_UNIMPLEMENTED(void, PR_DestroyCondVar, PRCondVar*);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_NotifyCondVar, PRCondVar*);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_NotifyAllCondVar, PRCondVar*);

MONGO_MOZ_UNIMPLEMENTED(uint32_t, PR_MillisecondsToInterval, uint32_t);

MONGO_MOZ_UNIMPLEMENTED(uint32_t, PR_MicrosecondsToInterval, uint32_t);

MONGO_MOZ_UNIMPLEMENTED(uint32_t, PR_TicksPerSecond);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_WaitCondVar, PRCondVar*, uint32_t);

MONGO_MOZ_UNIMPLEMENTED(int32_t, PR_FileDesc2NativeHandle, PRFileDesc*);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_GetOpenFileInfo, PRFileDesc*, PRFileInfo*);

MONGO_MOZ_UNIMPLEMENTED(int32_t, PR_Seek, PRFileDesc*, int32_t, PRSeekWhence);

MONGO_MOZ_UNIMPLEMENTED(PRFileMap*, PR_CreateFileMap, PRFileDesc*, int64_t, PRFileMapProtect);

MONGO_MOZ_UNIMPLEMENTED(void*, PR_MemMap, PRFileMap*, int64_t, uint32_t);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_MemUnmap, void*, uint32_t);

MONGO_MOZ_UNIMPLEMENTED(PRStatus, PR_CloseFileMap, PRFileMap*);
