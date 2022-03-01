// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 1999-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  icuinfo.cpp
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2009-2010
*   created by: Steven R. Loomis
*
*   This program shows some basic info about the current ICU.
*/

#include <stdio.h>
#include <stdlib.h>
#include "unicode/utypes.h"
#include "unicode/putil.h"
#include "unicode/uclean.h"
#include "udbgutil.h"
#include "unewdata.h"
#include "cmemory.h"
#include "cstring.h"
#include "uoptions.h"
#include "toolutil.h"
#include "icuplugimp.h"
#include <unicode/uloc.h>
#include <unicode/ucnv.h>
#include "unicode/ucal.h"
#include <unicode/ulocdata.h>
#include "putilimp.h"
#include "unicode/uchar.h"

static UOption options[]={
  /*0*/ UOPTION_HELP_H,
  /*1*/ UOPTION_HELP_QUESTION_MARK,
  /*2*/ UOPTION_ICUDATADIR,
  /*3*/ UOPTION_VERBOSE,
  /*4*/ UOPTION_DEF("list-plugins", 'L', UOPT_NO_ARG), // may be a no-op if disabled
  /*5*/ UOPTION_DEF("milisecond-time", 'm', UOPT_NO_ARG),
  /*6*/ UOPTION_DEF("cleanup", 'K', UOPT_NO_ARG),
  /*7*/ UOPTION_DEF("xml", 'x', UOPT_REQUIRES_ARG),
};

static UErrorCode initStatus = U_ZERO_ERROR;
static UBool icuInitted = FALSE;

static void do_init() {
    if(!icuInitted) {
      u_init(&initStatus);
      icuInitted = TRUE;
    }
}

static void do_cleanup() {
  if (icuInitted) {
    u_cleanup();
    icuInitted = FALSE;
  }
}

void cmd_millis()
{
  printf("Milliseconds since Epoch: %.0f\n", uprv_getUTCtime());
}

void cmd_version(UBool /* noLoad */, UErrorCode &errorCode)
{

    do_init();

    udbg_writeIcuInfo(stdout); /* print the XML format */
    
    union {
        uint8_t byte;
        uint16_t word;
    } u;
    u.word=0x0100;
    if(U_IS_BIG_ENDIAN==u.byte) {
      //printf("U_IS_BIG_ENDIAN: %d\n", U_IS_BIG_ENDIAN);
    } else {
        fprintf(stderr, "  error: U_IS_BIG_ENDIAN=%d != %d=actual 'is big endian'\n",
                U_IS_BIG_ENDIAN, u.byte);
        errorCode=U_INTERNAL_PROGRAM_ERROR;
    }

#if defined(_MSC_VER)
// Ignore warning 4127, conditional expression is constant. This is intentional below.
#pragma warning(push)
#pragma warning(disable: 4127)
#endif

    if(U_SIZEOF_WCHAR_T==sizeof(wchar_t)) {
      //printf("U_SIZEOF_WCHAR_T: %d\n", U_SIZEOF_WCHAR_T);
    } else {
        fprintf(stderr, "  error: U_SIZEOF_WCHAR_T=%d != %d=sizeof(wchar_t)\n",
                U_SIZEOF_WCHAR_T, (int)sizeof(wchar_t));
        errorCode=U_INTERNAL_PROGRAM_ERROR;
    }

    int charsetFamily;
    if('A'==0x41) {
        charsetFamily=U_ASCII_FAMILY;
    } else if('A'==0xc1) {
        charsetFamily=U_EBCDIC_FAMILY;
    } else {
        charsetFamily=-1;  // unknown
    }
    if(U_CHARSET_FAMILY==charsetFamily) {
      //printf("U_CHARSET_FAMILY: %d\n", U_CHARSET_FAMILY);
    } else {
        fprintf(stderr, "  error: U_CHARSET_FAMILY=%d != %d=actual charset family\n",
                U_CHARSET_FAMILY, charsetFamily);
        errorCode=U_INTERNAL_PROGRAM_ERROR;
    }

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    printf("\n\nICU Initialization returned: %s\n", u_errorName(initStatus));
    

#if UCONFIG_ENABLE_PLUGINS    
#if U_ENABLE_DYLOAD
    const char *pluginFile = uplug_getPluginFile();
    printf("Plugin file is: %s\n", (pluginFile&&*pluginFile)?pluginFile:"(not set. try setting ICU_PLUGINS to a directory.)");
#else
    fprintf(stderr, "Dynamic Loading: is disabled. No plugins will be loaded at start-up.\n");
#endif
#else
    fprintf(stderr, "Plugins are disabled.\n");
#endif    
}

void cmd_cleanup()
{
    u_cleanup();
    fprintf(stdout, "ICU u_cleanup() called.\n");
}


void cmd_listplugins() {
#if UCONFIG_ENABLE_PLUGINS
    int32_t i;
    UPlugData *plug;

    do_init();
    printf("ICU Initialized: u_init() returned %s\n", u_errorName(initStatus));
    
    printf("Plugins: \n");
    printf(    "# %6s   %s \n",
                       "Level",
                       "Name" );
    printf(    "    %10s:%-10s\n",
                       "Library",
                       "Symbol"
            );

                       
    printf(    "       config| (configuration string)\n");
    printf(    " >>>   Error          | Explanation \n");
    printf(    "-----------------------------------\n");
        
    for(i=0;(plug=uplug_getPlugInternal(i))!=NULL;i++) {
        UErrorCode libStatus = U_ZERO_ERROR;
        const char *name = uplug_getPlugName(plug);
        const char *sym = uplug_getSymbolName(plug);
        const char *lib = uplug_getLibraryName(plug, &libStatus);
        const char *config = uplug_getConfiguration(plug);
        UErrorCode loadStatus = uplug_getPlugLoadStatus(plug);
        const char *message = NULL;
        
        printf("\n#%d  %-6s %s \n",
            i+1,
            udbg_enumName(UDBG_UPlugLevel,(int32_t)uplug_getPlugLevel(plug)),
            name!=NULL?(*name?name:"this plugin did not call uplug_setPlugName()"):"(null)"
        );
        printf("    plugin| %10s:%-10s\n",
            (U_SUCCESS(libStatus)?(lib!=NULL?lib:"(null)"):u_errorName(libStatus)),
            sym!=NULL?sym:"(null)"
        );
        
        if(config!=NULL&&*config) {
            printf("    config| %s\n", config);
        }
        
        switch(loadStatus) {
            case U_PLUGIN_CHANGED_LEVEL_WARNING:
                message = "Note: This plugin changed the system level (by allocating memory or calling something which does). Later plugins may not load.";
                break;
                
            case U_PLUGIN_DIDNT_SET_LEVEL:
                message = "Error: This plugin did not call uplug_setPlugLevel during QUERY.";
                break;
            
            case U_PLUGIN_TOO_HIGH:
                message = "Error: This plugin couldn't load because the system level was too high. Try loading this plugin earlier.";
                break;
                
            case U_ZERO_ERROR: 
                message = NULL; /* no message */
                break;
            default:
                if(U_FAILURE(loadStatus)) {
                    message = "error loading:";
                } else {
                    message = "warning during load:";
                }            
        }
        
        if(message!=NULL) {
            printf("\\\\\\ status| %s\n"
                   "/// %s\n", u_errorName(loadStatus), message);
        }
        
    }
	if(i==0) {
		printf("No plugins loaded.\n");
	}
#endif
}



extern int
main(int argc, char* argv[]) {
    UErrorCode errorCode = U_ZERO_ERROR;
    UBool didSomething = FALSE;
    
    /* preset then read command line options */
    argc=u_parseArgs(argc, argv, UPRV_LENGTHOF(options), options);

    /* error handling, printing usage message */
    if(argc<0) {
        fprintf(stderr,
            "error in command line argument \"%s\"\n",
            argv[-argc]);
    }
    if( options[0].doesOccur || options[1].doesOccur) {
      fprintf(stderr, "%s: Output information about the current ICU\n", argv[0]);
      fprintf(stderr, "Options:\n"
              " -h     or  --help                 - Print this help message.\n"
              " -m     or  --millisecond-time     - Print the current UTC time in milliseconds.\n"
              " -d <dir>   or  --icudatadir <dir> - Set the ICU Data Directory\n"
              " -v                                - Print version and configuration information about ICU\n"
#if UCONFIG_ENABLE_PLUGINS
              " -L         or  --list-plugins     - List and diagnose issues with ICU Plugins\n"
#endif
              " -K         or  --cleanup          - Call u_cleanup() before exitting (will attempt to unload plugins)\n"
              "\n"
              "If no arguments are given, the tool will print ICU version and configuration information.\n"
              );
      fprintf(stderr, "International Components for Unicode %s\n%s\n", U_ICU_VERSION, U_COPYRIGHT_STRING );
      return argc<0 ? U_ILLEGAL_ARGUMENT_ERROR : U_ZERO_ERROR;
    }
    
    if(options[2].doesOccur) {
      u_setDataDirectory(options[2].value);
    }

    if(options[5].doesOccur) {
      cmd_millis();
      didSomething=TRUE;
    } 
    if(options[4].doesOccur) {
      cmd_listplugins();
      didSomething = TRUE;
    }

    if(options[3].doesOccur) {
      cmd_version(FALSE, errorCode);
      didSomething = TRUE;
    }

    if(options[7].doesOccur) {  /* 2nd part of version: cleanup */
      FILE *out = fopen(options[7].value, "w");
      if(out==NULL) {
        fprintf(stderr,"ERR: can't write to XML file %s\n", options[7].value);
        return 1;
      }
      /* todo: API for writing DTD? */
      fprintf(out, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n");
      udbg_writeIcuInfo(out);
      fclose(out);
      didSomething = TRUE;
    }

    if(options[6].doesOccur) {  /* 2nd part of version: cleanup */
      cmd_cleanup();
      didSomething = TRUE;
    }

    if(!didSomething) {
      cmd_version(FALSE, errorCode);  /* at least print the version # */
    }

    do_cleanup();

    return U_FAILURE(errorCode);
}
