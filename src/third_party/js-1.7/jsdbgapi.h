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

#ifndef jsdbgapi_h___
#define jsdbgapi_h___
/*
 * JS debugger API.
 */
#include "jsapi.h"
#include "jsopcode.h"
#include "jsprvtd.h"

JS_BEGIN_EXTERN_C

extern void
js_PatchOpcode(JSContext *cx, JSScript *script, jsbytecode *pc, JSOp op);

extern JS_PUBLIC_API(JSBool)
JS_SetTrap(JSContext *cx, JSScript *script, jsbytecode *pc,
           JSTrapHandler handler, void *closure);

extern JS_PUBLIC_API(JSOp)
JS_GetTrapOpcode(JSContext *cx, JSScript *script, jsbytecode *pc);

extern JS_PUBLIC_API(void)
JS_ClearTrap(JSContext *cx, JSScript *script, jsbytecode *pc,
             JSTrapHandler *handlerp, void **closurep);

extern JS_PUBLIC_API(void)
JS_ClearScriptTraps(JSContext *cx, JSScript *script);

extern JS_PUBLIC_API(void)
JS_ClearAllTraps(JSContext *cx);

extern JS_PUBLIC_API(JSTrapStatus)
JS_HandleTrap(JSContext *cx, JSScript *script, jsbytecode *pc, jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_SetInterrupt(JSRuntime *rt, JSTrapHandler handler, void *closure);

extern JS_PUBLIC_API(JSBool)
JS_ClearInterrupt(JSRuntime *rt, JSTrapHandler *handlerp, void **closurep);

/************************************************************************/

extern JS_PUBLIC_API(JSBool)
JS_SetWatchPoint(JSContext *cx, JSObject *obj, jsval id,
                 JSWatchPointHandler handler, void *closure);

extern JS_PUBLIC_API(JSBool)
JS_ClearWatchPoint(JSContext *cx, JSObject *obj, jsval id,
                   JSWatchPointHandler *handlerp, void **closurep);

extern JS_PUBLIC_API(JSBool)
JS_ClearWatchPointsForObject(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_ClearAllWatchPoints(JSContext *cx);

#ifdef JS_HAS_OBJ_WATCHPOINT
/*
 * Hide these non-API function prototypes by testing whether the internal
 * header file "jsconfig.h" has been included.
 */
extern void
js_MarkWatchPoints(JSContext *cx);

extern JSScopeProperty *
js_FindWatchPoint(JSRuntime *rt, JSScope *scope, jsid id);

extern JSPropertyOp
js_GetWatchedSetter(JSRuntime *rt, JSScope *scope,
                    const JSScopeProperty *sprop);

extern JSBool JS_DLL_CALLBACK
js_watch_set(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

extern JSBool JS_DLL_CALLBACK
js_watch_set_wrapper(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                     jsval *rval);

extern JSPropertyOp
js_WrapWatchedSetter(JSContext *cx, jsid id, uintN attrs, JSPropertyOp setter);

#endif /* JS_HAS_OBJ_WATCHPOINT */

/************************************************************************/

extern JS_PUBLIC_API(uintN)
JS_PCToLineNumber(JSContext *cx, JSScript *script, jsbytecode *pc);

extern JS_PUBLIC_API(jsbytecode *)
JS_LineNumberToPC(JSContext *cx, JSScript *script, uintN lineno);

extern JS_PUBLIC_API(JSScript *)
JS_GetFunctionScript(JSContext *cx, JSFunction *fun);

extern JS_PUBLIC_API(JSNative)
JS_GetFunctionNative(JSContext *cx, JSFunction *fun);

extern JS_PUBLIC_API(JSPrincipals *)
JS_GetScriptPrincipals(JSContext *cx, JSScript *script);

/*
 * Stack Frame Iterator
 *
 * Used to iterate through the JS stack frames to extract
 * information from the frames.
 */

extern JS_PUBLIC_API(JSStackFrame *)
JS_FrameIterator(JSContext *cx, JSStackFrame **iteratorp);

extern JS_PUBLIC_API(JSScript *)
JS_GetFrameScript(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(jsbytecode *)
JS_GetFramePC(JSContext *cx, JSStackFrame *fp);

/*
 * Get the closest scripted frame below fp.  If fp is null, start from cx->fp.
 */
extern JS_PUBLIC_API(JSStackFrame *)
JS_GetScriptedCaller(JSContext *cx, JSStackFrame *fp);

/*
 * Return a weak reference to fp's principals.  A null return does not denote
 * an error, it means there are no principals.
 */
extern JS_PUBLIC_API(JSPrincipals *)
JS_StackFramePrincipals(JSContext *cx, JSStackFrame *fp);

/*
 * This API is like JS_StackFramePrincipals(cx, caller), except that if
 * cx->runtime->findObjectPrincipals is non-null, it returns the weaker of
 * the caller's principals and the object principals of fp's callee function
 * object (fp->argv[-2]), which is eval, Function, or a similar eval-like
 * method.  The caller parameter should be JS_GetScriptedCaller(cx, fp).
 *
 * All eval-like methods must use JS_EvalFramePrincipals to acquire a weak
 * reference to the correct principals for the eval call to be secure, given
 * an embedding that calls JS_SetObjectPrincipalsFinder (see jsapi.h).
 */
extern JS_PUBLIC_API(JSPrincipals *)
JS_EvalFramePrincipals(JSContext *cx, JSStackFrame *fp, JSStackFrame *caller);

extern JS_PUBLIC_API(void *)
JS_GetFrameAnnotation(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(void)
JS_SetFrameAnnotation(JSContext *cx, JSStackFrame *fp, void *annotation);

extern JS_PUBLIC_API(void *)
JS_GetFramePrincipalArray(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(JSBool)
JS_IsNativeFrame(JSContext *cx, JSStackFrame *fp);

/* this is deprecated, use JS_GetFrameScopeChain instead */
extern JS_PUBLIC_API(JSObject *)
JS_GetFrameObject(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(JSObject *)
JS_GetFrameScopeChain(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(JSObject *)
JS_GetFrameCallObject(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(JSObject *)
JS_GetFrameThis(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(JSFunction *)
JS_GetFrameFunction(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(JSObject *)
JS_GetFrameFunctionObject(JSContext *cx, JSStackFrame *fp);

/* XXXrginda Initially published with typo */
#define JS_IsContructorFrame JS_IsConstructorFrame
extern JS_PUBLIC_API(JSBool)
JS_IsConstructorFrame(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(JSBool)
JS_IsDebuggerFrame(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(jsval)
JS_GetFrameReturnValue(JSContext *cx, JSStackFrame *fp);

extern JS_PUBLIC_API(void)
JS_SetFrameReturnValue(JSContext *cx, JSStackFrame *fp, jsval rval);

/**
 * Return fp's callee function object (fp->argv[-2]) if it has one.
 */
extern JS_PUBLIC_API(JSObject *)
JS_GetFrameCalleeObject(JSContext *cx, JSStackFrame *fp);

/************************************************************************/

extern JS_PUBLIC_API(const char *)
JS_GetScriptFilename(JSContext *cx, JSScript *script);

extern JS_PUBLIC_API(uintN)
JS_GetScriptBaseLineNumber(JSContext *cx, JSScript *script);

extern JS_PUBLIC_API(uintN)
JS_GetScriptLineExtent(JSContext *cx, JSScript *script);

extern JS_PUBLIC_API(JSVersion)
JS_GetScriptVersion(JSContext *cx, JSScript *script);

/************************************************************************/

/*
 * Hook setters for script creation and destruction, see jsprvtd.h for the
 * typedefs.  These macros provide binary compatibility and newer, shorter
 * synonyms.
 */
#define JS_SetNewScriptHook     JS_SetNewScriptHookProc
#define JS_SetDestroyScriptHook JS_SetDestroyScriptHookProc

extern JS_PUBLIC_API(void)
JS_SetNewScriptHook(JSRuntime *rt, JSNewScriptHook hook, void *callerdata);

extern JS_PUBLIC_API(void)
JS_SetDestroyScriptHook(JSRuntime *rt, JSDestroyScriptHook hook,
                        void *callerdata);

/************************************************************************/

extern JS_PUBLIC_API(JSBool)
JS_EvaluateUCInStackFrame(JSContext *cx, JSStackFrame *fp,
                          const jschar *chars, uintN length,
                          const char *filename, uintN lineno,
                          jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_EvaluateInStackFrame(JSContext *cx, JSStackFrame *fp,
                        const char *bytes, uintN length,
                        const char *filename, uintN lineno,
                        jsval *rval);

/************************************************************************/

typedef struct JSPropertyDesc {
    jsval           id;         /* primary id, a string or int */
    jsval           value;      /* property value */
    uint8           flags;      /* flags, see below */
    uint8           spare;      /* unused */
    uint16          slot;       /* argument/variable slot */
    jsval           alias;      /* alias id if JSPD_ALIAS flag */
} JSPropertyDesc;

#define JSPD_ENUMERATE  0x01    /* visible to for/in loop */
#define JSPD_READONLY   0x02    /* assignment is error */
#define JSPD_PERMANENT  0x04    /* property cannot be deleted */
#define JSPD_ALIAS      0x08    /* property has an alias id */
#define JSPD_ARGUMENT   0x10    /* argument to function */
#define JSPD_VARIABLE   0x20    /* local variable in function */
#define JSPD_EXCEPTION  0x40    /* exception occurred fetching the property, */
                                /* value is exception */
#define JSPD_ERROR      0x80    /* native getter returned JS_FALSE without */
                                /* throwing an exception */

typedef struct JSPropertyDescArray {
    uint32          length;     /* number of elements in array */
    JSPropertyDesc  *array;     /* alloc'd by Get, freed by Put */
} JSPropertyDescArray;

extern JS_PUBLIC_API(JSScopeProperty *)
JS_PropertyIterator(JSObject *obj, JSScopeProperty **iteratorp);

extern JS_PUBLIC_API(JSBool)
JS_GetPropertyDesc(JSContext *cx, JSObject *obj, JSScopeProperty *sprop,
                   JSPropertyDesc *pd);

extern JS_PUBLIC_API(JSBool)
JS_GetPropertyDescArray(JSContext *cx, JSObject *obj, JSPropertyDescArray *pda);

extern JS_PUBLIC_API(void)
JS_PutPropertyDescArray(JSContext *cx, JSPropertyDescArray *pda);

/************************************************************************/

extern JS_PUBLIC_API(JSBool)
JS_SetDebuggerHandler(JSRuntime *rt, JSTrapHandler handler, void *closure);

extern JS_PUBLIC_API(JSBool)
JS_SetSourceHandler(JSRuntime *rt, JSSourceHandler handler, void *closure);

extern JS_PUBLIC_API(JSBool)
JS_SetExecuteHook(JSRuntime *rt, JSInterpreterHook hook, void *closure);

extern JS_PUBLIC_API(JSBool)
JS_SetCallHook(JSRuntime *rt, JSInterpreterHook hook, void *closure);

extern JS_PUBLIC_API(JSBool)
JS_SetObjectHook(JSRuntime *rt, JSObjectHook hook, void *closure);

extern JS_PUBLIC_API(JSBool)
JS_SetThrowHook(JSRuntime *rt, JSTrapHandler hook, void *closure);

extern JS_PUBLIC_API(JSBool)
JS_SetDebugErrorHook(JSRuntime *rt, JSDebugErrorHook hook, void *closure);

/************************************************************************/

extern JS_PUBLIC_API(size_t)
JS_GetObjectTotalSize(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(size_t)
JS_GetFunctionTotalSize(JSContext *cx, JSFunction *fun);

extern JS_PUBLIC_API(size_t)
JS_GetScriptTotalSize(JSContext *cx, JSScript *script);

/*
 * Get the top-most running script on cx starting from fp, or from the top of
 * cx's frame stack if fp is null, and return its script filename flags.  If
 * the script has a null filename member, return JSFILENAME_NULL.
 */
extern JS_PUBLIC_API(uint32)
JS_GetTopScriptFilenameFlags(JSContext *cx, JSStackFrame *fp);

/*
 * Get the script filename flags for the script.  If the script doesn't have a
 * filename, return JSFILENAME_NULL.
 */
extern JS_PUBLIC_API(uint32)
JS_GetScriptFilenameFlags(JSScript *script);

/*
 * Associate flags with a script filename prefix in rt, so that any subsequent
 * script compilation will inherit those flags if the script's filename is the
 * same as prefix, or if prefix is a substring of the script's filename.
 *
 * The API defines only one flag bit, JSFILENAME_SYSTEM, leaving the remaining
 * 31 bits up to the API client to define.  The union of all 32 bits must not
 * be a legal combination, however, in order to preserve JSFILENAME_NULL as a
 * unique value.  API clients may depend on JSFILENAME_SYSTEM being a set bit
 * in JSFILENAME_NULL -- a script with a null filename member is presumed to
 * be a "system" script.
 */
extern JS_PUBLIC_API(JSBool)
JS_FlagScriptFilenamePrefix(JSRuntime *rt, const char *prefix, uint32 flags);

#define JSFILENAME_NULL         0xffffffff      /* null script filename */
#define JSFILENAME_SYSTEM       0x00000001      /* "system" script, see below */

/*
 * Return true if obj is a "system" object, that is, one flagged by a prior
 * call to JS_FlagSystemObject(cx, obj).  What "system" means is up to the API
 * client, but it can be used to coordinate access control policies based on
 * script filenames and their prefixes, using JS_FlagScriptFilenamePrefix and
 * JS_GetTopScriptFilenameFlags.
 */
extern JS_PUBLIC_API(JSBool)
JS_IsSystemObject(JSContext *cx, JSObject *obj);

/*
 * Flag obj as a "system" object.  The API client can flag system objects to
 * optimize access control checks.  The engine stores but does not interpret
 * the per-object flag set by this call.
 */
extern JS_PUBLIC_API(void)
JS_FlagSystemObject(JSContext *cx, JSObject *obj);

JS_END_EXTERN_C

#endif /* jsdbgapi_h___ */
