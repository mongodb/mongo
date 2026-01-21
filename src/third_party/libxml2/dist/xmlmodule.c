/*
 * xmlmodule.c : basic API for dynamic module loading added 2.6.17
 *
 * See Copyright for the status of this software.
 *
 * Author: Joel W. Reed
 *
 * http://www.fortran-2000.com/ArnaudRecipes/sharedlib.html
 */

/* In order RTLD_GLOBAL and RTLD_NOW to be defined on zOS */
#if defined(__MVS__)
#define _UNIX03_SOURCE
#endif

#define IN_LIBXML
#include "libxml.h"

#include <string.h>
#include <libxml/xmlmodule.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlstring.h>

#include "private/error.h"

#ifdef LIBXML_MODULES_ENABLED

struct _xmlModule {
    unsigned char *name;
    void *handle;
};

static void *xmlModulePlatformOpen(const char *name);
static int xmlModulePlatformClose(void *handle);
static int xmlModulePlatformSymbol(void *handle, const char *name, void **result);

/************************************************************************
 *									*
 *		module memory error handler				*
 *									*
 ************************************************************************/

/**
 * Opens a module/shared library given its name or path
 *
 * @deprecated Will be removed.
 *
 * NOTE: that due to portability issues, behaviour can only be
 * guaranteed with `name` using ASCII. We cannot guarantee that
 * an UTF-8 string would work, which is why name is a const char *
 * and not a const xmlChar * .
 * TODO: options are not yet implemented.
 *
 * @param name  the module name
 * @param options  a set of xmlModuleOption
 * @returns a handle for the module or NULL in case of error
 */
xmlModule *
xmlModuleOpen(const char *name, int options ATTRIBUTE_UNUSED)
{
    xmlModulePtr module;

    module = (xmlModulePtr) xmlMalloc(sizeof(xmlModule));
    if (module == NULL)
        return (NULL);

    memset(module, 0, sizeof(xmlModule));

    module->handle = xmlModulePlatformOpen(name);

    if (module->handle == NULL) {
        xmlFree(module);
        return(NULL);
    }

    module->name = xmlStrdup((const xmlChar *) name);
    return (module);
}

/**
 * Lookup for a symbol address in the given module
 *
 * @deprecated Will be removed.
 *
 * NOTE: that due to portability issues, behaviour can only be
 * guaranteed with `name` using ASCII. We cannot guarantee that
 * an UTF-8 string would work, which is why name is a const char *
 * and not a const xmlChar * .
 *
 * @param module  the module
 * @param name  the name of the symbol
 * @param symbol  the resulting symbol address
 * @returns 0 if the symbol was found, or -1 in case of error
 */
int
xmlModuleSymbol(xmlModule *module, const char *name, void **symbol)
{
    int rc = -1;

    if ((NULL == module) || (symbol == NULL) || (name == NULL))
        return rc;

    rc = xmlModulePlatformSymbol(module->handle, name, symbol);

    if (rc == -1)
        return rc;

    return rc;
}

/**
 * The close operations unload the associated module and free the
 * data associated to the module.
 *
 * @deprecated Will be removed.
 *
 * @param module  the module handle
 * @returns 0 in case of success, -1 in case of argument error and -2
 *         if the module could not be closed/unloaded.
 */
int
xmlModuleClose(xmlModule *module)
{
    int rc;

    if (NULL == module)
        return -1;

    rc = xmlModulePlatformClose(module->handle);

    if (rc != 0)
        return -2;

    rc = xmlModuleFree(module);
    return (rc);
}

/**
 * The free operations free the data associated to the module
 * but does not unload the associated shared library which may still
 * be in use.
 *
 * @deprecated Will be removed.
 *
 * @param module  the module handle
 * @returns 0 in case of success, -1 in case of argument error
 */
int
xmlModuleFree(xmlModule *module)
{
    if (NULL == module)
        return -1;

    xmlFree(module->name);
    xmlFree(module);

    return (0);
}

#if defined(HAVE_DLOPEN) && !defined(_WIN32)
#include <dlfcn.h>

#ifndef RTLD_GLOBAL            /* For Tru64 UNIX 4.0 */
#define RTLD_GLOBAL 0
#endif

/**
 * @param name  path to the module
 * @returns a handle on success, and zero on error.
 */

static void *
xmlModulePlatformOpen(const char *name)
{
    return dlopen(name, RTLD_GLOBAL | RTLD_NOW);
}

/*
 * @param handle  handle to the module
 *
 * @returns 0 on success, and non-zero on error.
 */

static int
xmlModulePlatformClose(void *handle)
{
    return dlclose(handle);
}

/*
 * http://www.opengroup.org/onlinepubs/009695399/functions/dlsym.html
 *
 * @returns 0 on success and the loaded symbol in result, and -1 on error.
 */

static int
xmlModulePlatformSymbol(void *handle, const char *name, void **symbol)
{
    *symbol = dlsym(handle, name);
    if (dlerror() != NULL) {
	return -1;
    }
    return 0;
}

#else /* ! HAVE_DLOPEN */

#ifdef HAVE_SHLLOAD             /* HAVE_SHLLOAD */
#include <dl.h>
/*
 * @returns a handle on success, and zero on error.
 */

static void *
xmlModulePlatformOpen(const char *name)
{
    return shl_load(name, BIND_IMMEDIATE, 0L);
}

/*
 * @returns 0 on success, and non-zero on error.
 */

static int
xmlModulePlatformClose(void *handle)
{
    return shl_unload(handle);
}

/*
 * http://docs.hp.com/en/B2355-90683/shl_load.3X.html
 *
 * @returns 0 on success and the loaded symbol in result, and -1 on error.
 */

static int
xmlModulePlatformSymbol(void *handle, const char *name, void **symbol)
{
    int rc;

    errno = 0;
    rc = shl_findsym(&handle, name, TYPE_UNDEFINED, symbol);
    return rc;
}

#endif /* HAVE_SHLLOAD */
#endif /* ! HAVE_DLOPEN */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * @returns a handle on success, and zero on error.
 */

static void *
xmlModulePlatformOpen(const char *name)
{
    return LoadLibraryA(name);
}

/*
 * @returns 0 on success, and non-zero on error.
 */

static int
xmlModulePlatformClose(void *handle)
{
    int rc;

    rc = FreeLibrary(handle);
    return (0 == rc);
}

/*
 * http://msdn.microsoft.com/library/default.asp?url=/library/en-us/dllproc/base/getprocaddress.asp
 *
 * @returns 0 on success and the loaded symbol in result, and -1 on error.
 */

static int
xmlModulePlatformSymbol(void *handle, const char *name, void **symbol)
{
    FARPROC proc = GetProcAddress(handle, name);

    memcpy(symbol, &proc, sizeof(proc));
    return (NULL == *symbol) ? -1 : 0;
}

#endif /* _WIN32 */

#endif /* LIBXML_MODULES_ENABLED */
