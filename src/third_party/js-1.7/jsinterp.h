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

#ifndef jsinterp_h___
#define jsinterp_h___
/*
 * JS interpreter interface.
 */
#include "jsprvtd.h"
#include "jspubtd.h"

JS_BEGIN_EXTERN_C

/*
 * JS stack frame, may be allocated on the C stack by native callers.  Always
 * allocated on cx->stackPool for calls from the interpreter to an interpreted
 * function.
 *
 * NB: This struct is manually initialized in jsinterp.c and jsiter.c.  If you
 * add new members, update both files.  But first, try to remove members.  The
 * sharp* and xml* members should be moved onto the stack as local variables
 * with well-known slots, if possible.
 */
struct JSStackFrame {
    JSObject        *callobj;       /* lazily created Call object */
    JSObject        *argsobj;       /* lazily created arguments object */
    JSObject        *varobj;        /* variables object, where vars go */
    JSScript        *script;        /* script being interpreted */
    JSFunction      *fun;           /* function being called or null */
    JSObject        *thisp;         /* "this" pointer if in method */
    uintN           argc;           /* actual argument count */
    jsval           *argv;          /* base of argument stack slots */
    jsval           rval;           /* function return value */
    uintN           nvars;          /* local variable count */
    jsval           *vars;          /* base of variable stack slots */
    JSStackFrame    *down;          /* previous frame */
    void            *annotation;    /* used by Java security */
    JSObject        *scopeChain;    /* scope chain */
    jsbytecode      *pc;            /* program counter */
    jsval           *sp;            /* stack pointer */
    jsval           *spbase;        /* operand stack base */
    uintN           sharpDepth;     /* array/object initializer depth */
    JSObject        *sharpArray;    /* scope for #n= initializer vars */
    uint32          flags;          /* frame flags -- see below */
    JSStackFrame    *dormantNext;   /* next dormant frame chain */
    JSObject        *xmlNamespace;  /* null or default xml namespace in E4X */
    JSObject        *blockChain;    /* active compile-time block scopes */
};

typedef struct JSInlineFrame {
    JSStackFrame    frame;          /* base struct */
    jsval           *rvp;           /* ptr to caller's return value slot */
    void            *mark;          /* mark before inline frame */
    void            *hookData;      /* debugger call hook data */
    JSVersion       callerVersion;  /* dynamic version of calling script */
} JSInlineFrame;

/* JS stack frame flags. */
#define JSFRAME_CONSTRUCTING  0x01  /* frame is for a constructor invocation */
#define JSFRAME_INTERNAL      0x02  /* internal call, not invoked by a script */
#define JSFRAME_SKIP_CALLER   0x04  /* skip one link when evaluating f.caller
                                       for this invocation of f */
#define JSFRAME_ASSIGNING     0x08  /* a complex (not simplex JOF_ASSIGNING) op
                                       is currently assigning to a property */
#define JSFRAME_DEBUGGER      0x10  /* frame for JS_EvaluateInStackFrame */
#define JSFRAME_EVAL          0x20  /* frame for obj_eval */
#define JSFRAME_SPECIAL       0x30  /* special evaluation frame flags */
#define JSFRAME_COMPILING     0x40  /* frame is being used by compiler */
#define JSFRAME_COMPILE_N_GO  0x80  /* compiler-and-go mode, can optimize name
                                       references based on scope chain */
#define JSFRAME_SCRIPT_OBJECT 0x100 /* compiling source for a Script object */
#define JSFRAME_YIELDING      0x200 /* js_Interpret dispatched JSOP_YIELD */
#define JSFRAME_FILTERING     0x400 /* XML filtering predicate expression */
#define JSFRAME_ITERATOR      0x800 /* trying to get an iterator for for-in */
#define JSFRAME_POP_BLOCKS   0x1000 /* scope chain contains blocks to pop */
#define JSFRAME_GENERATOR    0x2000 /* frame belongs to generator-iterator */

#define JSFRAME_OVERRIDE_SHIFT 24   /* override bit-set params; see jsfun.c */
#define JSFRAME_OVERRIDE_BITS  8

/*
 * Property cache for quickened get/set property opcodes.
 */
#define PROPERTY_CACHE_LOG2     10
#define PROPERTY_CACHE_SIZE     JS_BIT(PROPERTY_CACHE_LOG2)
#define PROPERTY_CACHE_MASK     JS_BITMASK(PROPERTY_CACHE_LOG2)

#define PROPERTY_CACHE_HASH(obj, id) \
    ((((jsuword)(obj) >> JSVAL_TAGBITS) ^ (jsuword)(id)) & PROPERTY_CACHE_MASK)

#ifdef JS_THREADSAFE

#if HAVE_ATOMIC_DWORD_ACCESS

#define PCE_LOAD(cache, pce, entry)     JS_ATOMIC_DWORD_LOAD(pce, entry)
#define PCE_STORE(cache, pce, entry)    JS_ATOMIC_DWORD_STORE(pce, entry)

#else  /* !HAVE_ATOMIC_DWORD_ACCESS */

#define JS_PROPERTY_CACHE_METERING      1

#define PCE_LOAD(cache, pce, entry)                                           \
    JS_BEGIN_MACRO                                                            \
        uint32 prefills_;                                                     \
        uint32 fills_ = (cache)->fills;                                       \
        do {                                                                  \
            /* Load until cache->fills is stable (see FILL macro below). */   \
            prefills_ = fills_;                                               \
            (entry) = *(pce);                                                 \
        } while ((fills_ = (cache)->fills) != prefills_);                     \
    JS_END_MACRO

#define PCE_STORE(cache, pce, entry)                                          \
    JS_BEGIN_MACRO                                                            \
        do {                                                                  \
            /* Store until no racing collider stores half or all of pce. */   \
            *(pce) = (entry);                                                 \
        } while (PCE_OBJECT(*pce) != PCE_OBJECT(entry) ||                     \
                 PCE_PROPERTY(*pce) != PCE_PROPERTY(entry));                  \
    JS_END_MACRO

#endif /* !HAVE_ATOMIC_DWORD_ACCESS */

#else  /* !JS_THREADSAFE */

#define PCE_LOAD(cache, pce, entry)     ((entry) = *(pce))
#define PCE_STORE(cache, pce, entry)    (*(pce) = (entry))

#endif /* !JS_THREADSAFE */

typedef union JSPropertyCacheEntry {
    struct {
        JSObject        *object;        /* weak link to object */
        JSScopeProperty *property;      /* weak link to property */
    } s;
#ifdef HAVE_ATOMIC_DWORD_ACCESS
    prdword align;
#endif
} JSPropertyCacheEntry;

/* These may be called in lvalue or rvalue position. */
#define PCE_OBJECT(entry)       ((entry).s.object)
#define PCE_PROPERTY(entry)     ((entry).s.property)

typedef struct JSPropertyCache {
    JSPropertyCacheEntry table[PROPERTY_CACHE_SIZE];
    JSBool               empty;
    JSBool               disabled;
#ifdef JS_PROPERTY_CACHE_METERING
    uint32               fills;
    uint32               recycles;
    uint32               tests;
    uint32               misses;
    uint32               flushes;
# define PCMETER(x)      x
#else
# define PCMETER(x)      /* nothing */
#endif
} JSPropertyCache;

#define PROPERTY_CACHE_FILL(cache, obj, id, sprop)                            \
    JS_BEGIN_MACRO                                                            \
        JSPropertyCache *cache_ = (cache);                                    \
        if (!cache_->disabled) {                                              \
            uintN hashIndex_ = (uintN) PROPERTY_CACHE_HASH(obj, id);          \
            JSPropertyCacheEntry *pce_ = &cache_->table[hashIndex_];          \
            JSPropertyCacheEntry entry_;                                      \
            JSScopeProperty *pce_sprop_;                                      \
            PCE_LOAD(cache_, pce_, entry_);                                   \
            pce_sprop_ = PCE_PROPERTY(entry_);                                \
            PCMETER(if (pce_sprop_ && pce_sprop_ != sprop)                    \
                        cache_->recycles++);                                  \
            PCE_OBJECT(entry_) = obj;                                         \
            PCE_PROPERTY(entry_) = sprop;                                     \
            cache_->empty = JS_FALSE;                                         \
            PCMETER(cache_->fills++);                                         \
            PCE_STORE(cache_, pce_, entry_);                                  \
        }                                                                     \
    JS_END_MACRO

#define PROPERTY_CACHE_TEST(cache, obj, id, sprop)                            \
    JS_BEGIN_MACRO                                                            \
        uintN hashIndex_ = (uintN) PROPERTY_CACHE_HASH(obj, id);              \
        JSPropertyCache *cache_ = (cache);                                    \
        JSPropertyCacheEntry *pce_ = &cache_->table[hashIndex_];              \
        JSPropertyCacheEntry entry_;                                          \
        JSScopeProperty *pce_sprop_;                                          \
        PCE_LOAD(cache_, pce_, entry_);                                       \
        pce_sprop_ = PCE_PROPERTY(entry_);                                    \
        PCMETER(cache_->tests++);                                             \
        if (pce_sprop_ &&                                                     \
            PCE_OBJECT(entry_) == obj &&                                      \
            pce_sprop_->id == id) {                                           \
            sprop = pce_sprop_;                                               \
        } else {                                                              \
            PCMETER(cache_->misses++);                                        \
            sprop = NULL;                                                     \
        }                                                                     \
    JS_END_MACRO

extern void
js_FlushPropertyCache(JSContext *cx);

extern void
js_DisablePropertyCache(JSContext *cx);

extern void
js_EnablePropertyCache(JSContext *cx);

extern JS_FRIEND_API(jsval *)
js_AllocStack(JSContext *cx, uintN nslots, void **markp);

extern JS_FRIEND_API(void)
js_FreeStack(JSContext *cx, void *mark);

extern JSBool
js_GetArgument(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

extern JSBool
js_SetArgument(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

extern JSBool
js_GetLocalVariable(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

extern JSBool
js_SetLocalVariable(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

#ifdef DUMP_CALL_TABLE
# define JSOPTION_LOGCALL_TOSOURCE JS_BIT(15)

extern JSHashTable  *js_CallTable;
extern size_t       js_LogCallToSourceLimit;

extern void         js_DumpCallTable(JSContext *cx);
#endif

/*
 * Refresh and return fp->scopeChain.  It may be stale if block scopes are
 * active but not yet reflected by objects in the scope chain.  If a block
 * scope contains a with, eval, XML filtering predicate, or similar such
 * dynamically scoped construct, then compile-time block scope at fp->blocks
 * must reflect at runtime.
 */
extern JSObject *
js_GetScopeChain(JSContext *cx, JSStackFrame *fp);

/*
 * Compute the 'this' parameter for a call with nominal 'this' given by thisp
 * and arguments including argv[-1] (nominal 'this') and argv[-2] (callee).
 * Activation objects ("Call" objects not created with "new Call()", i.e.,
 * "Call" objects that have private data) may not be referred to by 'this',
 * per ECMA-262, so js_ComputeThis censors them.
 */
extern JSObject *
js_ComputeThis(JSContext *cx, JSObject *thisp, jsval *argv);

/*
 * NB: js_Invoke requires that cx is currently running JS (i.e., that cx->fp
 * is non-null), and that the callee, |this| parameter, and actual arguments
 * are already pushed on the stack under cx->fp->sp.
 */
extern JS_FRIEND_API(JSBool)
js_Invoke(JSContext *cx, uintN argc, uintN flags);

/*
 * Consolidated js_Invoke flags simply rename certain JSFRAME_* flags, so that
 * we can share bits stored in JSStackFrame.flags and passed to:
 *
 *   js_Invoke
 *   js_InternalInvoke
 *   js_ValueToFunction
 *   js_ValueToFunctionObject
 *   js_ValueToCallableObject
 *   js_ReportIsNotFunction
 *
 * See jsfun.h for the latter four and flag renaming macros.
 */
#define JSINVOKE_CONSTRUCT      JSFRAME_CONSTRUCTING
#define JSINVOKE_INTERNAL       JSFRAME_INTERNAL
#define JSINVOKE_SKIP_CALLER    JSFRAME_SKIP_CALLER
#define JSINVOKE_ITERATOR       JSFRAME_ITERATOR

/*
 * Mask to isolate construct and iterator flags for use with jsfun.h functions.
 */
#define JSINVOKE_FUNFLAGS       (JSINVOKE_CONSTRUCT | JSINVOKE_ITERATOR)

/*
 * "Internal" calls may come from C or C++ code using a JSContext on which no
 * JS is running (!cx->fp), so they may need to push a dummy JSStackFrame.
 */
#define js_InternalCall(cx,obj,fval,argc,argv,rval)                           \
    js_InternalInvoke(cx, obj, fval, 0, argc, argv, rval)

#define js_InternalConstruct(cx,obj,fval,argc,argv,rval)                      \
    js_InternalInvoke(cx, obj, fval, JSINVOKE_CONSTRUCT, argc, argv, rval)

extern JSBool
js_InternalInvoke(JSContext *cx, JSObject *obj, jsval fval, uintN flags,
                  uintN argc, jsval *argv, jsval *rval);

extern JSBool
js_InternalGetOrSet(JSContext *cx, JSObject *obj, jsid id, jsval fval,
                    JSAccessMode mode, uintN argc, jsval *argv, jsval *rval);

extern JSBool
js_Execute(JSContext *cx, JSObject *chain, JSScript *script,
           JSStackFrame *down, uintN flags, jsval *result);

extern JSBool
js_CheckRedeclaration(JSContext *cx, JSObject *obj, jsid id, uintN attrs,
                      JSObject **objp, JSProperty **propp);

extern JSBool
js_StrictlyEqual(jsval lval, jsval rval);

extern JSBool
js_InvokeConstructor(JSContext *cx, jsval *vp, uintN argc);

extern JSBool
js_Interpret(JSContext *cx, jsbytecode *pc, jsval *result);

JS_END_EXTERN_C

#endif /* jsinterp_h___ */
