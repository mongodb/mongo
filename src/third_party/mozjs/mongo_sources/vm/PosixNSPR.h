/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PosixNSPR_h
#define vm_PosixNSPR_h

#include <stdint.h>
#include "jspubtd.h"

namespace nspr {
class Thread;
class Lock;
class CondVar;
};

typedef nspr::Thread PRThread;
typedef nspr::Lock PRLock;
typedef nspr::CondVar PRCondVar;

namespace mongo {
namespace mozjs {
void PR_BindThread(PRThread* thread);
PRThread* PR_CreateFakeThread();
void PR_DestroyFakeThread(PRThread* thread);
}  // namespace mozjs
}  // namespace mongo

enum PRThreadType {
   PR_USER_THREAD,
   PR_SYSTEM_THREAD
};

enum PRThreadPriority
{
   PR_PRIORITY_FIRST   = 0,
   PR_PRIORITY_LOW     = 0,
   PR_PRIORITY_NORMAL  = 1,
   PR_PRIORITY_HIGH    = 2,
   PR_PRIORITY_URGENT  = 3,
   PR_PRIORITY_LAST    = 3
};

enum PRThreadScope {
   PR_LOCAL_THREAD,
   PR_GLOBAL_THREAD,
   PR_GLOBAL_BOUND_THREAD
};

enum PRThreadState {
   PR_JOINABLE_THREAD,
   PR_UNJOINABLE_THREAD
};

PRThread*
PR_CreateThread(PRThreadType type,
                void (*start)(void* arg),
                void* arg,
                PRThreadPriority priority,
                PRThreadScope scope,
                PRThreadState state,
                uint32_t stackSize);

typedef enum { PR_FAILURE = -1, PR_SUCCESS = 0 } PRStatus;

PRStatus
PR_JoinThread(PRThread* thread);

PRThread*
PR_GetCurrentThread();

PRStatus
PR_SetCurrentThreadName(const char* name);

typedef void (*PRThreadPrivateDTOR)(void* priv);

PRStatus
PR_NewThreadPrivateIndex(unsigned* newIndex, PRThreadPrivateDTOR destructor);

PRStatus
PR_SetThreadPrivate(unsigned index, void* priv);

void*
PR_GetThreadPrivate(unsigned index);

struct PRCallOnceType {
    int initialized;
    int32_t inProgress;
    PRStatus status;
};

typedef PRStatus (*PRCallOnceFN)();

PRStatus
PR_CallOnce(PRCallOnceType* once, PRCallOnceFN func);

typedef PRStatus (*PRCallOnceWithArgFN)(void*);

PRStatus
PR_CallOnceWithArg(PRCallOnceType* once, PRCallOnceWithArgFN func, void* arg);

PRLock*
PR_NewLock();

void
PR_DestroyLock(PRLock* lock);

void
PR_Lock(PRLock* lock);

PRStatus
PR_Unlock(PRLock* lock);

PRCondVar*
PR_NewCondVar(PRLock* lock);

void
PR_DestroyCondVar(PRCondVar* cvar);

PRStatus
PR_NotifyCondVar(PRCondVar* cvar);

PRStatus
PR_NotifyAllCondVar(PRCondVar* cvar);

#define PR_INTERVAL_MIN 1000UL
#define PR_INTERVAL_MAX 100000UL

#define PR_INTERVAL_NO_WAIT 0UL
#define PR_INTERVAL_NO_TIMEOUT 0xffffffffUL

uint32_t
PR_MillisecondsToInterval(uint32_t milli);

uint32_t
PR_MicrosecondsToInterval(uint32_t micro);

uint32_t
PR_TicksPerSecond();

PRStatus
PR_WaitCondVar(PRCondVar* cvar, uint32_t timeout);

int32_t
PR_FileDesc2NativeHandle(PRFileDesc* fd);

enum PRFileType
{
    PR_FILE_FILE = 1,
    PR_FILE_DIRECTORY = 2,
    PR_FILE_OTHER = 3
};

struct PRFileInfo
{
    PRFileType type;
    int32_t size;
    int64_t creationTime;
    int64_t modifyTime;
};

PRStatus
PR_GetOpenFileInfo(PRFileDesc *fd, PRFileInfo *info);

enum PRSeekWhence
{
    PR_SEEK_SET = 0,
    PR_SEEK_CUR = 1,
    PR_SEEK_END = 2
};

int32_t
PR_Seek(PRFileDesc *fd, int32_t offset, PRSeekWhence whence);

enum PRFileMapProtect
{
    PR_PROT_READONLY,
    PR_PROT_READWRITE,
    PR_PROT_WRITECOPY
};

struct PRFileMap;

PRFileMap*
PR_CreateFileMap(PRFileDesc *fd, int64_t size, PRFileMapProtect prot);

void*
PR_MemMap(PRFileMap *fmap, int64_t offset, uint32_t len);

PRStatus
PR_MemUnmap(void *addr, uint32_t len);

PRStatus
PR_CloseFileMap(PRFileMap *fmap);

#endif /* vm_PosixNSPR_h */
