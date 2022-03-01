// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
******************************************************************************
*
*   Copyright (C) 2009-2015, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
******************************************************************************
*
*  FILE NAME : testplug.c
*
*   Date         Name        Description
*   10/29/2009   srl          New.
******************************************************************************
*
*
* This file implements a number of example ICU plugins. 
*
*/

#include "unicode/icuplug.h"

#if UCONFIG_ENABLE_PLUGINS
/* This file isn't usually compiled except on Windows. Guard it. */

#include <stdio.h> /* for fprintf */
#include <stdlib.h> /* for malloc */
#include "udbgutil.h"
#include "unicode/uclean.h"
#include "cmemory.h"

/**
 * Prototypes
 */
#define DECLARE_PLUGIN(x) U_CAPI UPlugTokenReturn U_EXPORT2 x (UPlugData *data, UPlugReason reason, UErrorCode *status)

DECLARE_PLUGIN(myPlugin);
DECLARE_PLUGIN(myPluginLow);
DECLARE_PLUGIN(myPluginFailQuery);
DECLARE_PLUGIN(myPluginFailToken);
DECLARE_PLUGIN(myPluginBad);
DECLARE_PLUGIN(myPluginHigh);
DECLARE_PLUGIN(debugMemoryPlugin);

/**
 * A simple, trivial plugin.
 */

U_CAPI
UPlugTokenReturn U_EXPORT2 myPlugin (
                  UPlugData *data,
                  UPlugReason reason,
                  UErrorCode *status) {
	/* Just print this for debugging */
    fprintf(stderr,"MyPlugin: data=%p, reason=%s, status=%s\n", (void*)data, udbg_enumName(UDBG_UPlugReason,(int32_t)reason), u_errorName(*status));

    if(reason==UPLUG_REASON_QUERY) {
        uplug_setPlugName(data, "Just a Test High-Level Plugin"); /* This call is optional in response to UPLUG_REASON_QUERY, but is a good idea. */
        uplug_setPlugLevel(data, UPLUG_LEVEL_HIGH); /* This call is Mandatory in response to UPLUG_REASON_QUERY */
    }

    return UPLUG_TOKEN; /* This must always be returned, to indicate that the entrypoint was actually a plugin. */
}


U_CAPI
UPlugTokenReturn U_EXPORT2 myPluginLow (
                  UPlugData *data,
                  UPlugReason reason,
                  UErrorCode *status) {
    fprintf(stderr,"MyPluginLow: data=%p, reason=%s, status=%s\n", (void*)data, udbg_enumName(UDBG_UPlugReason,(int32_t)reason), u_errorName(*status));

    if(reason==UPLUG_REASON_QUERY) {
        uplug_setPlugName(data, "Low Plugin");
        uplug_setPlugLevel(data, UPLUG_LEVEL_LOW);
    }

    return UPLUG_TOKEN;
}

/**
 * Doesn't respond to QUERY properly.
 */
U_CAPI
UPlugTokenReturn U_EXPORT2 myPluginFailQuery (
                  UPlugData *data,
                  UPlugReason reason,
                  UErrorCode *status) {
    fprintf(stderr,"MyPluginFailQuery: data=%p, reason=%s, status=%s\n", (void*)data, udbg_enumName(UDBG_UPlugReason,(int32_t)reason), u_errorName(*status));

	/* Should respond to UPLUG_REASON_QUERY here. */

    return UPLUG_TOKEN;
}

/**
 * Doesn't return the proper token.
 */
U_CAPI
UPlugTokenReturn U_EXPORT2 myPluginFailToken (
                  UPlugData *data,
                  UPlugReason reason,
                  UErrorCode *status) {
    fprintf(stderr,"MyPluginFailToken: data=%p, reason=%s, status=%s\n", (void*)data, udbg_enumName(UDBG_UPlugReason,(int32_t)reason), u_errorName(*status));

    if(reason==UPLUG_REASON_QUERY) {
        uplug_setPlugName(data, "myPluginFailToken Plugin");
        uplug_setPlugLevel(data, UPLUG_LEVEL_LOW);
    }

    return 0; /* Wrong. */
}



/**
 * Says it's low, but isn't.
 */
U_CAPI
UPlugTokenReturn U_EXPORT2 myPluginBad (
                  UPlugData *data,
                  UPlugReason reason,
                  UErrorCode *status) {
    fprintf(stderr,"MyPluginLow: data=%p, reason=%s, status=%s\n", (void*)data, udbg_enumName(UDBG_UPlugReason,(int32_t)reason), u_errorName(*status));

    if(reason==UPLUG_REASON_QUERY) {
        uplug_setPlugName(data, "Bad Plugin");
        uplug_setPlugLevel(data, UPLUG_LEVEL_LOW);
    } else if(reason == UPLUG_REASON_LOAD) {
        void *ctx = uprv_malloc(12345);
        
        uplug_setContext(data, ctx);
        fprintf(stderr,"I'm %p and I did a bad thing and malloced %p\n", (void*)data, (void*)ctx);
    } else if(reason == UPLUG_REASON_UNLOAD) {
        void * ctx = uplug_getContext(data);
        
        uprv_free(ctx);
    }
    

    return UPLUG_TOKEN;
}

U_CAPI 
UPlugTokenReturn U_EXPORT2 myPluginHigh (
                  UPlugData *data,
                  UPlugReason reason,
                  UErrorCode *status) {
    fprintf(stderr,"MyPluginHigh: data=%p, reason=%s, status=%s\n", (void*)data, udbg_enumName(UDBG_UPlugReason,(int32_t)reason), u_errorName(*status));

    if(reason==UPLUG_REASON_QUERY) {
        uplug_setPlugName(data, "High Plugin");
        uplug_setPlugLevel(data, UPLUG_LEVEL_HIGH);
    }

    return UPLUG_TOKEN;
}


/*  Debug Memory Plugin (see hpmufn.c) */
static void * U_CALLCONV myMemAlloc(const void *context, size_t size) {
  void *retPtr = (void *)malloc(size);
  (void)context; /* unused */
  fprintf(stderr, "MEM: malloc(%d) = %p\n", (int32_t)size, retPtr);
  return retPtr;
}

static void U_CALLCONV myMemFree(const void *context, void *mem) {
  (void)context; /* unused */

  free(mem);
  fprintf(stderr, "MEM: free(%p)\n", mem);
}

static void * U_CALLCONV myMemRealloc(const void *context, void *mem, size_t size) {
    void *retPtr;
    (void)context; /* unused */

    
    if(mem==NULL) {
        retPtr = NULL;
    } else {
        retPtr = realloc(mem, size);
    }
    fprintf(stderr, "MEM: realloc(%p, %d) = %p\n", mem, (int32_t)size, retPtr);
    return retPtr;
}

U_CAPI
UPlugTokenReturn U_EXPORT2 debugMemoryPlugin (
                  UPlugData *data,
                  UPlugReason reason,
                  UErrorCode *status) {
    fprintf(stderr,"debugMemoryPlugin: data=%p, reason=%s, status=%s\n", (void*)data, udbg_enumName(UDBG_UPlugReason,(int32_t)reason), u_errorName(*status));
    
    if(reason==UPLUG_REASON_QUERY) {
        uplug_setPlugLevel(data, UPLUG_LEVEL_LOW);
        uplug_setPlugName(data, "Memory Plugin");
    } else if(reason==UPLUG_REASON_LOAD) {
        u_setMemoryFunctions(uplug_getContext(data), &myMemAlloc, &myMemRealloc, &myMemFree, status);
        fprintf(stderr, "MEM: status now %s\n", u_errorName(*status));
    } else if(reason==UPLUG_REASON_UNLOAD) {
        fprintf(stderr, "MEM: not possible to unload this plugin (no way to reset memory functions)...\n");
        uplug_setPlugNoUnload(data, TRUE);
    }

    return UPLUG_TOKEN;
}

#endif
