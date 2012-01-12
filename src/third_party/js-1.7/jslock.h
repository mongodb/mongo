/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
#ifndef jslock_h__
#define jslock_h__

#ifdef JS_THREADSAFE

#include "jstypes.h"
#include "pratom.h"
#include "prlock.h"
#include "prcvar.h"
#include "prthread.h"

#include "jsprvtd.h"    /* for JSScope, etc. */
#include "jspubtd.h"    /* for JSRuntime, etc. */

#define Thin_GetWait(W) ((jsword)(W) & 0x1)
#define Thin_SetWait(W) ((jsword)(W) | 0x1)
#define Thin_RemoveWait(W) ((jsword)(W) & ~0x1)

typedef struct JSFatLock JSFatLock;

struct JSFatLock {
    int         susp;
    PRLock      *slock;
    PRCondVar   *svar;
    JSFatLock   *next;
    JSFatLock   **prevp;
};

typedef struct JSThinLock {
    jsword      owner;
    JSFatLock   *fat;
} JSThinLock;

#define CX_THINLOCK_ID(cx)       ((jsword)(cx)->thread)
#define CURRENT_THREAD_IS_ME(me) (((JSThread *)me)->id == js_CurrentThreadId())

typedef PRLock JSLock;

typedef struct JSFatLockTable {
    JSFatLock   *free;
    JSFatLock   *taken;
} JSFatLockTable;

/*
 * Atomic increment and decrement for a reference counter, given jsrefcount *p.
 * NB: jsrefcount is int32, aka PRInt32, so that pratom.h functions work.
 */
#define JS_ATOMIC_INCREMENT(p)      PR_AtomicIncrement((PRInt32 *)(p))
#define JS_ATOMIC_DECREMENT(p)      PR_AtomicDecrement((PRInt32 *)(p))
#define JS_ATOMIC_ADD(p,v)          PR_AtomicAdd((PRInt32 *)(p), (PRInt32)(v))

#define js_CurrentThreadId()        (jsword)PR_GetCurrentThread()
#define JS_NEW_LOCK()               PR_NewLock()
#define JS_DESTROY_LOCK(l)          PR_DestroyLock(l)
#define JS_ACQUIRE_LOCK(l)          PR_Lock(l)
#define JS_RELEASE_LOCK(l)          PR_Unlock(l)
#define JS_LOCK0(P,M)               js_Lock(P,M)
#define JS_UNLOCK0(P,M)             js_Unlock(P,M)

#define JS_NEW_CONDVAR(l)           PR_NewCondVar(l)
#define JS_DESTROY_CONDVAR(cv)      PR_DestroyCondVar(cv)
#define JS_WAIT_CONDVAR(cv,to)      PR_WaitCondVar(cv,to)
#define JS_NO_TIMEOUT               PR_INTERVAL_NO_TIMEOUT
#define JS_NOTIFY_CONDVAR(cv)       PR_NotifyCondVar(cv)
#define JS_NOTIFY_ALL_CONDVAR(cv)   PR_NotifyAllCondVar(cv)

/*
 * Include jsscope.h so JS_LOCK_OBJ macro callers don't have to include it.
 * Since there is a JSThinLock member in JSScope, we can't nest this include
 * much earlier (see JSThinLock's typedef, above).  Yes, that means there is
 * an #include cycle between jslock.h and jsscope.h: moderate-sized XXX here,
 * to be fixed by moving JS_LOCK_SCOPE to jsscope.h, JS_LOCK_OBJ to jsobj.h,
 * and so on.
 */
#include "jsscope.h"

#define JS_LOCK_RUNTIME(rt)         js_LockRuntime(rt)
#define JS_UNLOCK_RUNTIME(rt)       js_UnlockRuntime(rt)

/*
 * NB: The JS_LOCK_OBJ and JS_UNLOCK_OBJ macros work *only* on native objects
 * (objects for which OBJ_IS_NATIVE returns true).  All uses of these macros in
 * the engine are predicated on OBJ_IS_NATIVE or equivalent checks.  These uses
 * are for optimizations above the JSObjectOps layer, under which object locks
 * normally hide.
 */
#define JS_LOCK_OBJ(cx,obj)         ((OBJ_SCOPE(obj)->ownercx == (cx))        \
                                     ? (void)0                                \
                                     : (js_LockObj(cx, obj)))
#define JS_UNLOCK_OBJ(cx,obj)       ((OBJ_SCOPE(obj)->ownercx == (cx))        \
                                     ? (void)0 : js_UnlockObj(cx, obj))

#define JS_LOCK_SCOPE(cx,scope)     ((scope)->ownercx == (cx) ? (void)0       \
                                     : js_LockScope(cx, scope))
#define JS_UNLOCK_SCOPE(cx,scope)   ((scope)->ownercx == (cx) ? (void)0       \
                                     : js_UnlockScope(cx, scope))
#define JS_TRANSFER_SCOPE_LOCK(cx, scope, newscope)                           \
                                    js_TransferScopeLock(cx, scope, newscope)

extern void js_LockRuntime(JSRuntime *rt);
extern void js_UnlockRuntime(JSRuntime *rt);
extern void js_LockObj(JSContext *cx, JSObject *obj);
extern void js_UnlockObj(JSContext *cx, JSObject *obj);
extern void js_LockScope(JSContext *cx, JSScope *scope);
extern void js_UnlockScope(JSContext *cx, JSScope *scope);
extern int js_SetupLocks(int,int);
extern void js_CleanupLocks();
extern void js_TransferScopeLock(JSContext *, JSScope *, JSScope *);
extern JS_FRIEND_API(jsval)
js_GetSlotThreadSafe(JSContext *, JSObject *, uint32);
extern void js_SetSlotThreadSafe(JSContext *, JSObject *, uint32, jsval);
extern void js_InitLock(JSThinLock *);
extern void js_FinishLock(JSThinLock *);
extern void js_FinishSharingScope(JSRuntime *rt, JSScope *scope);

#ifdef DEBUG

#define JS_IS_RUNTIME_LOCKED(rt)        js_IsRuntimeLocked(rt)
#define JS_IS_OBJ_LOCKED(cx,obj)        js_IsObjLocked(cx,obj)
#define JS_IS_SCOPE_LOCKED(cx,scope)    js_IsScopeLocked(cx,scope)

extern JSBool js_IsRuntimeLocked(JSRuntime *rt);
extern JSBool js_IsObjLocked(JSContext *cx, JSObject *obj);
extern JSBool js_IsScopeLocked(JSContext *cx, JSScope *scope);

#else

#define JS_IS_RUNTIME_LOCKED(rt)        0
#define JS_IS_OBJ_LOCKED(cx,obj)        1
#define JS_IS_SCOPE_LOCKED(cx,scope)    1

#endif /* DEBUG */

#define JS_LOCK_OBJ_VOID(cx, obj, e)                                          \
    JS_BEGIN_MACRO                                                            \
        JS_LOCK_OBJ(cx, obj);                                                 \
        e;                                                                    \
        JS_UNLOCK_OBJ(cx, obj);                                               \
    JS_END_MACRO

#define JS_LOCK_VOID(cx, e)                                                   \
    JS_BEGIN_MACRO                                                            \
        JSRuntime *_rt = (cx)->runtime;                                       \
        JS_LOCK_RUNTIME_VOID(_rt, e);                                         \
    JS_END_MACRO

/* FIXME: bug 353962 hackaround */
#define JS_USE_ONLY_NSPR_LOCKS  1

#if defined(JS_USE_ONLY_NSPR_LOCKS) ||                                        \
    !( (defined(_WIN32) && defined(_M_IX86)) ||                               \
       (defined(__GNUC__) && defined(__i386__)) ||                            \
       ((defined(__USLC__) || defined(_SCO_DS)) && defined(i386)) ||          \
       (defined(SOLARIS) && defined(sparc) && defined(ULTRA_SPARC)) ||        \
       defined(AIX) )

#define NSPR_LOCK 1

#undef JS_LOCK0
#undef JS_UNLOCK0
#define JS_LOCK0(P,M)   (JS_ACQUIRE_LOCK(((JSLock*)(P)->fat)), (P)->owner = (M))
#define JS_UNLOCK0(P,M) ((P)->owner = 0, JS_RELEASE_LOCK(((JSLock*)(P)->fat)))

#else  /* arch-tests */

#undef NSPR_LOCK

extern JS_INLINE void js_Lock(JSThinLock *tl, jsword me);
extern JS_INLINE void js_Unlock(JSThinLock *tl, jsword me);

#endif /* arch-tests */

#else  /* !JS_THREADSAFE */

#define JS_ATOMIC_INCREMENT(p)      (++*(p))
#define JS_ATOMIC_DECREMENT(p)      (--*(p))
#define JS_ATOMIC_ADD(p,v)          (*(p) += (v))

#define JS_CurrentThreadId() 0
#define JS_NEW_LOCK()               NULL
#define JS_DESTROY_LOCK(l)          ((void)0)
#define JS_ACQUIRE_LOCK(l)          ((void)0)
#define JS_RELEASE_LOCK(l)          ((void)0)
#define JS_LOCK0(P,M)               ((void)0)
#define JS_UNLOCK0(P,M)             ((void)0)

#define JS_NEW_CONDVAR(l)           NULL
#define JS_DESTROY_CONDVAR(cv)      ((void)0)
#define JS_WAIT_CONDVAR(cv,to)      ((void)0)
#define JS_NOTIFY_CONDVAR(cv)       ((void)0)
#define JS_NOTIFY_ALL_CONDVAR(cv)   ((void)0)

#define JS_LOCK_RUNTIME(rt)         ((void)0)
#define JS_UNLOCK_RUNTIME(rt)       ((void)0)
#define JS_LOCK_OBJ(cx,obj)         ((void)0)
#define JS_UNLOCK_OBJ(cx,obj)       ((void)0)
#define JS_LOCK_OBJ_VOID(cx,obj,e)  (e)
#define JS_LOCK_SCOPE(cx,scope)     ((void)0)
#define JS_UNLOCK_SCOPE(cx,scope)   ((void)0)
#define JS_TRANSFER_SCOPE_LOCK(c,o,n) ((void)0)

#define JS_IS_RUNTIME_LOCKED(rt)        1
#define JS_IS_OBJ_LOCKED(cx,obj)        1
#define JS_IS_SCOPE_LOCKED(cx,scope)    1
#define JS_LOCK_VOID(cx, e)             JS_LOCK_RUNTIME_VOID((cx)->runtime, e)

#endif /* !JS_THREADSAFE */

#define JS_LOCK_RUNTIME_VOID(rt,e)                                            \
    JS_BEGIN_MACRO                                                            \
        JS_LOCK_RUNTIME(rt);                                                  \
        e;                                                                    \
        JS_UNLOCK_RUNTIME(rt);                                                \
    JS_END_MACRO

#define JS_LOCK_GC(rt)              JS_ACQUIRE_LOCK((rt)->gcLock)
#define JS_UNLOCK_GC(rt)            JS_RELEASE_LOCK((rt)->gcLock)
#define JS_LOCK_GC_VOID(rt,e)       (JS_LOCK_GC(rt), (e), JS_UNLOCK_GC(rt))
#define JS_AWAIT_GC_DONE(rt)        JS_WAIT_CONDVAR((rt)->gcDone, JS_NO_TIMEOUT)
#define JS_NOTIFY_GC_DONE(rt)       JS_NOTIFY_ALL_CONDVAR((rt)->gcDone)
#define JS_AWAIT_REQUEST_DONE(rt)   JS_WAIT_CONDVAR((rt)->requestDone,        \
                                                    JS_NO_TIMEOUT)
#define JS_NOTIFY_REQUEST_DONE(rt)  JS_NOTIFY_CONDVAR((rt)->requestDone)

#define JS_LOCK(P,CX)               JS_LOCK0(P, CX_THINLOCK_ID(CX))
#define JS_UNLOCK(P,CX)             JS_UNLOCK0(P, CX_THINLOCK_ID(CX))

#endif /* jslock_h___ */
