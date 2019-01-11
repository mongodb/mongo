/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "util/NativeStack.h"

#ifdef XP_WIN
# include "util/Windows.h"
#elif defined(XP_DARWIN) || defined(DARWIN) || defined(XP_UNIX)
# include <pthread.h>
# if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#  include <pthread_np.h>
# endif
# if defined(ANDROID) && !defined(__aarch64__)
#  include <sys/types.h>
#  include <unistd.h>
# endif
#else
# error "Unsupported platform"
#endif

#include "jsfriendapi.h"

#if defined(XP_WIN)

void*
js::GetNativeStackBaseImpl()
{
    PNT_TIB pTib = reinterpret_cast<PNT_TIB>(NtCurrentTeb());
    return static_cast<void*>(pTib->StackBase);
}

#elif defined(SOLARIS)

#include <ucontext.h>

JS_STATIC_ASSERT(JS_STACK_GROWTH_DIRECTION < 0);

void*
js::GetNativeStackBaseImpl()
{
    stack_t st;
    stack_getbounds(&st);
    return static_cast<char*>(st.ss_sp) + st.ss_size;
}

#elif defined(AIX)

#include <ucontext.h>

JS_STATIC_ASSERT(JS_STACK_GROWTH_DIRECTION < 0);

void*
js::GetNativeStackBaseImpl()
{
    ucontext_t context;
    getcontext(&context);
    return static_cast<char*>(context.uc_stack.ss_sp) +
        context.uc_stack.ss_size;
}

#else /* XP_UNIX */

void*
js::GetNativeStackBaseImpl()
{
    pthread_t thread = pthread_self();
# if defined(XP_DARWIN) || defined(DARWIN)
    return pthread_get_stackaddr_np(thread);

# else
    pthread_attr_t sattr;
    pthread_attr_init(&sattr);
#  if defined(__OpenBSD__)
    stack_t ss;
#  elif defined(PTHREAD_NP_H) || defined(_PTHREAD_NP_H_) || defined(NETBSD)
    /* e.g. on FreeBSD 4.8 or newer, neundorf@kde.org */
    pthread_attr_get_np(thread, &sattr);
#  else
    /*
     * FIXME: this function is non-portable;
     * other POSIX systems may have different np alternatives
     */
    pthread_getattr_np(thread, &sattr);
#  endif

    void* stackBase = 0;
    size_t stackSize = 0;
    int rc;
# if defined(__OpenBSD__)
    rc = pthread_stackseg_np(pthread_self(), &ss);
    stackBase = (void*)((size_t) ss.ss_sp - ss.ss_size);
    stackSize = ss.ss_size;
# elif defined(ANDROID) && !defined(__aarch64__)
    if (gettid() == getpid()) {
        // bionic's pthread_attr_getstack prior to API 21 doesn't tell the truth
        // for the main thread (see bug 846670). So we scan /proc/self/maps to
        // find the segment which contains the stack.
        rc = -1;

        // Put the string on the stack, otherwise there is the danger that it
        // has not been decompressed by the the on-demand linker. Bug 1165460.
        //
        // The volatile keyword should stop the compiler from trying to omit
        // the stack copy in the future (hopefully).
        volatile char path[] = "/proc/self/maps";
        FILE* fs = fopen((const char*)path, "r");

        if (fs) {
            char line[100];
            unsigned long stackAddr = (unsigned long)&sattr;
            while (fgets(line, sizeof(line), fs) != nullptr) {
                unsigned long stackStart;
                unsigned long stackEnd;
                if (sscanf(line, "%lx-%lx ", &stackStart, &stackEnd) == 2 &&
                    stackAddr >= stackStart && stackAddr < stackEnd) {
                    stackBase = (void*)stackStart;
                    stackSize = stackEnd - stackStart;
                    rc = 0;
                    break;
                }
            }
            fclose(fs);
        }
    } else
        // For non main-threads pthread allocates the stack itself so it tells
        // the truth.
        rc = pthread_attr_getstack(&sattr, &stackBase, &stackSize);
# else
    rc = pthread_attr_getstack(&sattr, &stackBase, &stackSize);
# endif
    if (rc)
        MOZ_CRASH();
    MOZ_ASSERT(stackBase);
    pthread_attr_destroy(&sattr);

#  if JS_STACK_GROWTH_DIRECTION > 0
    return stackBase;
#  else
    return static_cast<char*>(stackBase) + stackSize;
#  endif
# endif
}

#endif /* !XP_WIN */
