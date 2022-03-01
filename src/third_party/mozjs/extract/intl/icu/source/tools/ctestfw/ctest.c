// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
********************************************************************************
*
*   Copyright (C) 1996-2014, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
********************************************************************************
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <ctype.h>

#include "unicode/utrace.h"
#include "unicode/uclean.h"
#include "putilimp.h"
#include "udbgutil.h"

/* NOTES:
   3/20/1999 srl - strncpy called w/o setting nulls at the end
 */

#define MAXTESTNAME 128
#define MAXTESTS  512
#define MAX_TEST_LOG 4096

/**
 *  How may columns to indent the 'OK' markers.
 */
#define FLAG_INDENT 45
/**
 *   How many lines of scrollage can go by before we need to remind the user what the test is.
 */
#define PAGE_SIZE_LIMIT 25

#ifndef SHOW_TIMES
#define SHOW_TIMES 1
#endif

struct TestNode
{
  void (*test)(void);
  struct TestNode* sibling;
  struct TestNode* child;
  char name[1]; /* This is dynamically allocated off the end with malloc. */
};


static const struct TestNode* currentTest;

typedef enum { RUNTESTS, SHOWTESTS } TestMode;
#define TEST_SEPARATOR '/'

#ifndef C_TEST_IMPL
#define C_TEST_IMPL
#endif

#include "unicode/ctest.h"

static char ERROR_LOG[MAX_TEST_LOG][MAXTESTNAME];

/* Local prototypes */
static TestNode* addTestNode( TestNode *root, const char *name );

static TestNode *createTestNode(const char* name, int32_t nameLen);

static int strncmp_nullcheck( const char* s1,
                  const char* s2,
                  int n );

static void getNextLevel( const char* name,
              int* nameLen,
              const char** nextName );

static void iterateTestsWithLevel( const TestNode *root, int depth,
                   const TestNode** nodeList,
                   TestMode mode);

static void help ( const char *argv0 );

/**
 * Do the work of logging an error. Doesn't increase the error count.
 *
 * @prefix optional prefix prepended to message, or NULL.
 * @param pattern printf style pattern
 * @param ap vprintf style arg list
 */
static void vlog_err(const char *prefix, const char *pattern, va_list ap);
static void vlog_verbose(const char *prefix, const char *pattern, va_list ap);
static UBool vlog_knownIssue(const char *ticket, const char *pattern, va_list ap);

/**
 * Log test structure, with indent
 * @param pattern printf pattern
 */
static void log_testinfo_i(const char *pattern, ...);

/**
 * Log test structure, NO indent
 * @param pattern printf pattern
 */
static void log_testinfo(const char *pattern, ...);

/* If we need to make the framework multi-thread safe
   we need to pass around the following vars
*/
static int ERRONEOUS_FUNCTION_COUNT = 0;
static int ERROR_COUNT = 0; /* Count of errors from all tests. */
static int ONE_ERROR = 0; /* were there any other errors? */
static int DATA_ERROR_COUNT = 0; /* count of data related errors or warnings */
static int INDENT_LEVEL = 0;
static UBool NO_KNOWN = FALSE;
static void *knownList = NULL;
static char gTestName[1024] = "";
static UBool ON_LINE = FALSE; /* are we on the top line with our test name? */
static UBool HANGING_OUTPUT = FALSE; /* did the user leave us without a trailing \n ? */
static int GLOBAL_PRINT_COUNT = 0; /* global count of printouts */
int REPEAT_TESTS_INIT = 0; /* Was REPEAT_TESTS initialized? */
int REPEAT_TESTS = 1; /* Number of times to run the test */
int VERBOSITY = 0; /* be No-verbose by default */
int ERR_MSG =1; /* error messages will be displayed by default*/
int QUICK = 1;  /* Skip some of the slower tests? */
int WARN_ON_MISSING_DATA = 0; /* Reduce data errs to warnings? */
UTraceLevel ICU_TRACE = UTRACE_OFF;  /* ICU tracing level */
size_t MINIMUM_MEMORY_SIZE_FAILURE = (size_t)-1; /* Minimum library memory allocation window that will fail. */
size_t MAXIMUM_MEMORY_SIZE_FAILURE = (size_t)-1; /* Maximum library memory allocation window that will fail. */
static const char *ARGV_0 = "[ALL]";
static const char *XML_FILE_NAME=NULL;
static char XML_PREFIX[256];
static const char *SUMMARY_FILE = NULL;
FILE *XML_FILE = NULL;
/*-------------------------------------------*/

/* strncmp that also makes sure there's a \0 at s2[0] */
static int strncmp_nullcheck( const char* s1,
               const char* s2,
               int n )
{
    if (((int)strlen(s2) >= n) && s2[n] != 0) {
        return 3; /* null check fails */
    }
    else {
        return strncmp ( s1, s2, n );
    }
}

static void getNextLevel( const char* name,
           int* nameLen,
           const char** nextName )
{
    /* Get the next component of the name */
    *nextName = strchr(name, TEST_SEPARATOR);

    if( *nextName != 0 )
    {
        char n[255];
        *nameLen = (int)((*nextName) - name);
        (*nextName)++; /* skip '/' */
        strncpy(n, name, *nameLen);
        n[*nameLen] = 0;
        /*printf("->%s-< [%d] -> [%s]\n", name, *nameLen, *nextName);*/
    }
    else {
        *nameLen = (int)strlen(name);
    }
}

static TestNode *createTestNode(const char* name, int32_t nameLen)
{
    TestNode *newNode;

    newNode = (TestNode*)malloc(sizeof(TestNode) + (nameLen + 1));

    newNode->test = NULL;
    newNode->sibling = NULL;
    newNode->child = NULL;

    strncpy( newNode->name, name, nameLen );
    newNode->name[nameLen] = 0;

    return  newNode;
}

void T_CTEST_EXPORT2
cleanUpTestTree(TestNode *tn)
{
    if(tn->child != NULL) {
        cleanUpTestTree(tn->child);
    }
    if(tn->sibling != NULL) {
        cleanUpTestTree(tn->sibling);
    }

    free(tn);

}


void T_CTEST_EXPORT2
addTest(TestNode** root,
        TestFunctionPtr test,
        const char* name )
{
    TestNode *newNode;

    /*if this is the first Test created*/
    if (*root == NULL)
        *root = createTestNode("", 0);

    newNode = addTestNode( *root, name );
    assert(newNode != 0 );
    /*  printf("addTest: nreName = %s\n", newNode->name );*/

    newNode->test = test;
}

/* non recursive insert function */
static TestNode *addTestNode ( TestNode *root, const char *name )
{
    const char* nextName;
    TestNode *nextNode, *curNode;
    int nameLen; /* length of current 'name' */

    /* remove leading slash */
    if ( *name == TEST_SEPARATOR )
        name++;

    curNode = root;

    for(;;)
    {
        /* Start with the next child */
        nextNode = curNode->child;

        getNextLevel ( name, &nameLen, &nextName );

        /*      printf("* %s\n", name );*/

        /* if nextNode is already null, then curNode has no children
        -- add them */
        if( nextNode == NULL )
        {
            /* Add all children of the node */
            do
            {
                /* Get the next component of the name */
                getNextLevel(name, &nameLen, &nextName);

                /* update curName to have the next name segment */
                curNode->child = createTestNode(name, nameLen);
                /* printf("*** added %s\n", curNode->child->name );*/
                curNode = curNode->child;
                name = nextName;
            }
            while( name != NULL );

            return curNode;
        }

        /* Search across for the name */
        while (strncmp_nullcheck ( name, nextNode->name, nameLen) != 0 )
        {
            curNode = nextNode;
            nextNode = nextNode -> sibling;

            if ( nextNode == NULL )
            {
                /* Did not find 'name' on this level. */
                nextNode = createTestNode(name, nameLen);
                curNode->sibling = nextNode;
                break;
            }
        }

        /* nextNode matches 'name' */

        if (nextName == NULL) /* end of the line */
        {
            return nextNode;
        }

        /* Loop again with the next item */
        name = nextName;
        curNode = nextNode;
    }
}

/**
 * Log the time taken. May not output anything.
 * @param deltaTime change in time
 */
void T_CTEST_EXPORT2 str_timeDelta(char *str, UDate deltaTime) {
  if (deltaTime > 110000.0 ) {
    double mins = uprv_floor(deltaTime/60000.0);
    sprintf(str, "[(%.0fm %.1fs)]", mins, (deltaTime-(mins*60000.0))/1000.0);
  } else if (deltaTime > 1500.0) {
    sprintf(str, "((%.1fs))", deltaTime/1000.0);
  } else if(deltaTime>900.0) {
    sprintf(str, "( %.2fs )", deltaTime/1000.0);
  } else if(deltaTime > 5.0) {
    sprintf(str, " (%.0fms) ", deltaTime);
  } else {
    str[0]=0; /* at least terminate it. */
  }
}

static void print_timeDelta(UDate deltaTime) {
  char str[256];
  str_timeDelta(str, deltaTime);
  if(str[0]) {
    printf("%s", str);
  }
}

/**
 * Run or list tests (according to mode) in a subtree.
 *
 * @param root root of the subtree to operate on
 * @param depth The depth of this tree (0=root)
 * @param nodeList an array of MAXTESTS depth that's used for keeping track of where we are. nodeList[depth] points to the 'parent' at depth depth.
 * @param mode what mode we are operating in.
 */
static void iterateTestsWithLevel ( const TestNode* root,
                 int depth,
                 const TestNode** nodeList,
                 TestMode mode)
{
    int i;

    char pathToFunction[MAXTESTNAME] = "";
    char separatorString[2] = { TEST_SEPARATOR, '\0'};
#if SHOW_TIMES
    UDate allStartTime = -1, allStopTime = -1;
#endif

    if(depth<2) {
      allStartTime = uprv_getRawUTCtime();
    }

    if ( root == NULL )
        return;

    /* record the current root node, and increment depth. */
    nodeList[depth++] = root;
    /* depth is now the depth of root's children. */

    /* Collect the 'path' to the current subtree. */
    for ( i=0;i<(depth-1);i++ )
    {
        strcat(pathToFunction, nodeList[i]->name);
        strcat(pathToFunction, separatorString);
    }
    strcat(pathToFunction, nodeList[i]->name); /* including 'root' */

    /* print test name and space. */
    INDENT_LEVEL = depth-1;
    if(root->name[0]) {
    	log_testinfo_i("%s ", root->name);
    } else {
    	log_testinfo_i("(%s) ", ARGV_0);
    }
    ON_LINE = TRUE;  /* we are still on the line with the test name */


    if ( (mode == RUNTESTS) &&
		(root->test != NULL))  /* if root is a leaf node, run it */
    {
        int myERROR_COUNT = ERROR_COUNT;
        int myGLOBAL_PRINT_COUNT = GLOBAL_PRINT_COUNT;
#if SHOW_TIMES
        UDate startTime, stopTime;
        char timeDelta[256];
        char timeSeconds[256];
#else 
        const char timeDelta[] = "(unknown)";
        const char timeSeconds[] = "0.000";
#endif
        currentTest = root;
        INDENT_LEVEL = depth;  /* depth of subitems */
        ONE_ERROR=0;
        HANGING_OUTPUT=FALSE;
#if SHOW_TIMES
        startTime = uprv_getRawUTCtime();
#endif
        strcpy(gTestName, pathToFunction);
        root->test();   /* PERFORM THE TEST ************************/
#if SHOW_TIMES
        stopTime = uprv_getRawUTCtime();
#endif
        if(HANGING_OUTPUT) {
          log_testinfo("\n");
          HANGING_OUTPUT=FALSE;
        }
        INDENT_LEVEL = depth-1;  /* depth of root */
        currentTest = NULL;
        if((ONE_ERROR>0)&&(ERROR_COUNT==0)) {
          ERROR_COUNT++; /* There was an error without a newline */
        }
        ONE_ERROR=0;

#if SHOW_TIMES
        str_timeDelta(timeDelta, stopTime-startTime);
        sprintf(timeSeconds, "%f", (stopTime-startTime)/1000.0);
#endif        
        ctest_xml_testcase(pathToFunction, pathToFunction, timeSeconds, (myERROR_COUNT!=ERROR_COUNT)?"error":NULL);

        if (myERROR_COUNT != ERROR_COUNT) {
          log_testinfo_i("} ---[%d ERRORS in %s] ", ERROR_COUNT - myERROR_COUNT, pathToFunction);
          strcpy(ERROR_LOG[ERRONEOUS_FUNCTION_COUNT++], pathToFunction);
        } else {
          if(!ON_LINE) { /* had some output */
            int spaces = FLAG_INDENT-(depth-1);
            log_testinfo_i("} %*s[OK] ", spaces, "---");
            if((GLOBAL_PRINT_COUNT-myGLOBAL_PRINT_COUNT)>PAGE_SIZE_LIMIT) {
              log_testinfo(" %s ", pathToFunction); /* in case they forgot. */
            }
          } else {
            /* put -- out at 30 sp. */
            int spaces = FLAG_INDENT - ((int)strlen(root->name) + depth);
            if(spaces<0) spaces=0;
            log_testinfo(" %*s[OK] ", spaces,"---");
          }
        }
                           
#if SHOW_TIMES
        if(timeDelta[0]) printf("%s", timeDelta);
#endif
         
        ON_LINE = TRUE; /* we are back on-line */
    }

    INDENT_LEVEL = depth-1; /* root */

    /* we want these messages to be at 0 indent. so just push the indent level breifly. */
    if(mode==SHOWTESTS) {
    	log_testinfo("---%s%c\n",pathToFunction, nodeList[i]->test?' ':TEST_SEPARATOR );
    }

    INDENT_LEVEL = depth;

    if(root->child) {
        int myERROR_COUNT = ERROR_COUNT;
        int myGLOBAL_PRINT_COUNT = GLOBAL_PRINT_COUNT;
        if(mode!=SHOWTESTS) {
    		INDENT_LEVEL=depth-1;
    		log_testinfo("{\n");
    		INDENT_LEVEL=depth;
    	}

    	iterateTestsWithLevel ( root->child, depth, nodeList, mode );

    	if(mode!=SHOWTESTS) {
    		INDENT_LEVEL=depth-1;
    		log_testinfo_i("} "); /* TODO:  summarize subtests */
    		if((depth>1) && (ERROR_COUNT > myERROR_COUNT)) {
    			log_testinfo("[%d %s in %s] ", ERROR_COUNT-myERROR_COUNT, (ERROR_COUNT-myERROR_COUNT)==1?"error":"errors", pathToFunction);
    		} else if((GLOBAL_PRINT_COUNT-myGLOBAL_PRINT_COUNT)>PAGE_SIZE_LIMIT || (depth<1)) {
                  if(pathToFunction[0]) {
                    log_testinfo(" %s ", pathToFunction); /* in case they forgot. */
                  } else {
                    log_testinfo(" / (%s) ", ARGV_0);
                  }
                }

    		ON_LINE=TRUE;
    	}
	}
    depth--;

#if SHOW_TIMES
    if(depth<2) {
      allStopTime = uprv_getRawUTCtime();
      print_timeDelta(allStopTime-allStartTime);
    }
#endif

    if(mode!=SHOWTESTS && ON_LINE) {
    	log_testinfo("\n");
    }

    if ( depth != 0 ) { /* DO NOT iterate over siblings of the root. TODO: why not? */
        iterateTestsWithLevel ( root->sibling, depth, nodeList, mode );
    }
}



void T_CTEST_EXPORT2
showTests ( const TestNode *root )
{
    /* make up one for them */
    const TestNode *nodeList[MAXTESTS];

    if (root == NULL)
        log_err("TEST CAN'T BE FOUND!");

    iterateTestsWithLevel ( root, 0, nodeList, SHOWTESTS );

}

void T_CTEST_EXPORT2
runTests ( const TestNode *root )
{
    int i;
    const TestNode *nodeList[MAXTESTS];
    /* make up one for them */


    if (root == NULL)
        log_err("TEST CAN'T BE FOUND!\n");

    ERRONEOUS_FUNCTION_COUNT = ERROR_COUNT = 0;
    iterateTestsWithLevel ( root, 0, nodeList, RUNTESTS );

    /*print out result summary*/

    ON_LINE=FALSE; /* just in case */

    if(knownList != NULL) {
      if( udbg_knownIssue_print(knownList) ) {
        fprintf(stdout, "(To run suppressed tests, use the -K option.) \n\n");
      }
      udbg_knownIssue_close(knownList);
      knownList = NULL;
    }

    if (ERROR_COUNT)
    {
        fprintf(stdout,"\nSUMMARY:\n");
    	fflush(stdout);
        fprintf(stdout,"******* [Total error count:\t%d]\n", ERROR_COUNT);
    	fflush(stdout);
        fprintf(stdout, " Errors in\n");
        for (i=0;i < ERRONEOUS_FUNCTION_COUNT; i++)
            fprintf(stdout, "[%s]\n",ERROR_LOG[i]);
	if(SUMMARY_FILE != NULL) {
	  FILE *summf = fopen(SUMMARY_FILE, "w");
	  if(summf!=NULL) {
	    for (i=0;i < ERRONEOUS_FUNCTION_COUNT; i++)
	      fprintf(summf, "%s\n",ERROR_LOG[i]);
	    fclose(summf);
	  }
	}
    }
    else
    {
      log_testinfo("\n[All tests passed successfully...]\n");
    }

    if(DATA_ERROR_COUNT) {
      if(WARN_ON_MISSING_DATA==0) {
    	  log_testinfo("\t*Note* some errors are data-loading related. If the data used is not the \n"
                 "\tstock ICU data (i.e some have been added or removed), consider using\n"
                 "\tthe '-w' option to turn these errors into warnings.\n");
      } else {
    	  log_testinfo("\t*WARNING* some data-loading errors were ignored by the -w option.\n");
      }
    }
}

const char* T_CTEST_EXPORT2
getTestName(void)
{
  if(currentTest != NULL) {
    return currentTest->name;
  } else {
    return NULL;
  }
}

const TestNode* T_CTEST_EXPORT2
getTest(const TestNode* root, const char* name)
{
    const char* nextName;
    TestNode *nextNode;
    const TestNode* curNode;
    int nameLen; /* length of current 'name' */

    if (root == NULL) {
        log_err("TEST CAN'T BE FOUND!\n");
        return NULL;
    }
    /* remove leading slash */
    if ( *name == TEST_SEPARATOR )
        name++;

    curNode = root;

    for(;;)
    {
        /* Start with the next child */
        nextNode = curNode->child;

        getNextLevel ( name, &nameLen, &nextName );

        /*      printf("* %s\n", name );*/

        /* if nextNode is already null, then curNode has no children
        -- add them */
        if( nextNode == NULL )
        {
            return NULL;
        }

        /* Search across for the name */
        while (strncmp_nullcheck ( name, nextNode->name, nameLen) != 0 )
        {
            curNode = nextNode;
            nextNode = nextNode -> sibling;

            if ( nextNode == NULL )
            {
                /* Did not find 'name' on this level. */
                return NULL;
            }
        }

        /* nextNode matches 'name' */

        if (nextName == NULL) /* end of the line */
        {
            return nextNode;
        }

        /* Loop again with the next item */
        name = nextName;
        curNode = nextNode;
    }
}

/*  =========== io functions ======== */

static void go_offline_with_marker(const char *mrk) {
  UBool wasON_LINE = ON_LINE;
  
  if(ON_LINE) {
    log_testinfo(" {\n");
    ON_LINE=FALSE;
  }
  
  if(!HANGING_OUTPUT || wasON_LINE) {
    if(mrk != NULL) {
      fputs(mrk, stdout);
    }
  }
}

static void go_offline() {
	go_offline_with_marker(NULL);
}

static void go_offline_err() {
	go_offline();
}

static void first_line_verbose() {
    go_offline_with_marker("v");
}

static void first_line_err() {
    go_offline_with_marker("!");
}

static void first_line_info() {
    go_offline_with_marker("\"");
}

static void first_line_test() {
	fputs(" ", stdout);
}


static void vlog_err(const char *prefix, const char *pattern, va_list ap)
{
    if( ERR_MSG == FALSE){
        return;
    }
    fputs("!", stdout); /* col 1 - bang */
    fprintf(stdout, "%-*s", INDENT_LEVEL,"" );
    if(prefix) {
        fputs(prefix, stdout);
    }
    vfprintf(stdout, pattern, ap);
    fflush(stdout);
    va_end(ap);
    if((*pattern==0) || (pattern[strlen(pattern)-1]!='\n')) {
    	HANGING_OUTPUT=1;
    } else {
    	HANGING_OUTPUT=0;
    }
    GLOBAL_PRINT_COUNT++;
}

static UBool vlog_knownIssue(const char *ticket, const char *pattern, va_list ap)
{
    char buf[2048];
    UBool firstForTicket;
    UBool firstForWhere;

    if(NO_KNOWN) return FALSE;
    if(pattern==NULL) pattern="";

    vsprintf(buf, pattern, ap);
    knownList = udbg_knownIssue_open(knownList, ticket, gTestName, buf,
                                     &firstForTicket, &firstForWhere);

    if(firstForTicket || firstForWhere) {
      log_info("(Known issue %s) %s\n", ticket, buf);
    } else {
      log_verbose("(Known issue %s) %s\n", ticket, buf);
    }

    return TRUE;
}


void T_CTEST_EXPORT2
vlog_info(const char *prefix, const char *pattern, va_list ap)
{
	first_line_info();
    fprintf(stdout, "%-*s", INDENT_LEVEL,"" );
    if(prefix) {
        fputs(prefix, stdout);
    }
    vfprintf(stdout, pattern, ap);
    fflush(stdout);
    va_end(ap);
    if((*pattern==0) || (pattern[strlen(pattern)-1]!='\n')) {
    	HANGING_OUTPUT=1;
    } else {
    	HANGING_OUTPUT=0;
    }
    GLOBAL_PRINT_COUNT++;
}
/**
 * Log test structure, with indent
 */
static void log_testinfo_i(const char *pattern, ...)
{
    va_list ap;
    first_line_test();
    fprintf(stdout, "%-*s", INDENT_LEVEL,"" );
    va_start(ap, pattern);
    vfprintf(stdout, pattern, ap);
    fflush(stdout);
    va_end(ap);
    GLOBAL_PRINT_COUNT++;
}
/**
 * Log test structure (no ident)
 */
static void log_testinfo(const char *pattern, ...)
{
    va_list ap;
    va_start(ap, pattern);
    first_line_test();
    vfprintf(stdout, pattern, ap);
    fflush(stdout);
    va_end(ap);
    GLOBAL_PRINT_COUNT++;
}


static void vlog_verbose(const char *prefix, const char *pattern, va_list ap)
{
    if ( VERBOSITY == FALSE )
        return;

    first_line_verbose();
    fprintf(stdout, "%-*s", INDENT_LEVEL,"" );
    if(prefix) {
        fputs(prefix, stdout);
    }
    vfprintf(stdout, pattern, ap);
    fflush(stdout);
    va_end(ap);
    GLOBAL_PRINT_COUNT++;
    if((*pattern==0) || (pattern[strlen(pattern)-1]!='\n')) {
    	HANGING_OUTPUT=1;
    } else {
    	HANGING_OUTPUT=0;
    }
}

void T_CTEST_EXPORT2
log_err(const char* pattern, ...)
{
    va_list ap;
    first_line_err();
    if(strchr(pattern, '\n') != NULL) {
        /*
         * Count errors only if there is a line feed in the pattern
         * so that we do not exaggerate our error count.
         */
        ++ERROR_COUNT;
    } else {
    	/* Count at least one error. */
    	ONE_ERROR=1;
    }
    va_start(ap, pattern);
    vlog_err(NULL, pattern, ap);
}

UBool T_CTEST_EXPORT2
log_knownIssue(const char *ticket, const char *pattern, ...) {
  va_list ap;
  va_start(ap, pattern);
  return vlog_knownIssue(ticket, pattern, ap);
}

void T_CTEST_EXPORT2
log_err_status(UErrorCode status, const char* pattern, ...)
{
    va_list ap;
    va_start(ap, pattern);
    
    if ((status == U_FILE_ACCESS_ERROR || status == U_MISSING_RESOURCE_ERROR)) {
        ++DATA_ERROR_COUNT; /* for informational message at the end */
        
        if (WARN_ON_MISSING_DATA == 0) {
            first_line_err();
            /* Fatal error. */
            if (strchr(pattern, '\n') != NULL) {
                ++ERROR_COUNT;
            } else {
                ++ONE_ERROR;
            }
            vlog_err(NULL, pattern, ap); /* no need for prefix in default case */
        } else {
            vlog_info("[DATA] ", pattern, ap); 
        }
    } else {
        first_line_err();
        /* Fatal error. */
        if(strchr(pattern, '\n') != NULL) {
            ++ERROR_COUNT;
        } else {
            ++ONE_ERROR;
        }
        vlog_err(NULL, pattern, ap); /* no need for prefix in default case */
    }
}

void T_CTEST_EXPORT2
log_info(const char* pattern, ...)
{
    va_list ap;

    va_start(ap, pattern);
    vlog_info(NULL, pattern, ap);
}

void T_CTEST_EXPORT2
log_verbose(const char* pattern, ...)
{
    va_list ap;

    va_start(ap, pattern);
    vlog_verbose(NULL, pattern, ap);
}


void T_CTEST_EXPORT2
log_data_err(const char* pattern, ...)
{
    va_list ap;
    va_start(ap, pattern);

    go_offline_err();
    ++DATA_ERROR_COUNT; /* for informational message at the end */

    if(WARN_ON_MISSING_DATA == 0) {
        /* Fatal error. */
        if(strchr(pattern, '\n') != NULL) {
            ++ERROR_COUNT;
        }
        vlog_err(NULL, pattern, ap); /* no need for prefix in default case */
    } else {
        vlog_info("[DATA] ", pattern, ap); 
    }
}


/*
 * Tracing functions.
 */
static int traceFnNestingDepth = 0;
U_CDECL_BEGIN
static void U_CALLCONV TraceEntry(const void *context, int32_t fnNumber) {
    (void)context; // suppress compiler warnings about unused variable
    char buf[500];
    utrace_format(buf, sizeof(buf), traceFnNestingDepth*3, "%s() enter.\n", utrace_functionName(fnNumber));
    buf[sizeof(buf)-1]=0;  
    fputs(buf, stdout);
    traceFnNestingDepth++;
}   
 
static void U_CALLCONV TraceExit(const void *context, int32_t fnNumber, const char *fmt, va_list args) {
    (void)context; // suppress compiler warnings about unused variable
    char buf[500];
    if (traceFnNestingDepth>0) {
        traceFnNestingDepth--; 
    }
    utrace_format(buf, sizeof(buf), traceFnNestingDepth*3, "%s() ", utrace_functionName(fnNumber));
    buf[sizeof(buf)-1]=0;
    fputs(buf, stdout);
    utrace_vformat(buf, sizeof(buf), traceFnNestingDepth*3, fmt, args);
    buf[sizeof(buf)-1]=0;
    fputs(buf, stdout);
    putc('\n', stdout);
}

static void U_CALLCONV TraceData(const void *context, int32_t fnNumber,
                          int32_t level, const char *fmt, va_list args) {
    // suppress compiler warnings about unused variables
    (void)context;  
    (void)fnNumber;
    (void)level;
    char buf[500];
    utrace_vformat(buf, sizeof(buf), traceFnNestingDepth*3, fmt, args);
    buf[sizeof(buf)-1]=0;
    fputs(buf, stdout);
    putc('\n', stdout);
}

static void *U_CALLCONV ctest_libMalloc(const void *context, size_t size) {
    (void)context; // suppress compiler warnings about unused variable
    /*if (VERBOSITY) {
        printf("Allocated %ld\n", (long)size);
    }*/
    if (MINIMUM_MEMORY_SIZE_FAILURE <= size && size <= MAXIMUM_MEMORY_SIZE_FAILURE) {
        return NULL;
    }
    return malloc(size);
}
static void *U_CALLCONV ctest_libRealloc(const void *context, void *mem, size_t size) {
    (void)context; // suppress compiler warnings about unused variable
    /*if (VERBOSITY) {
        printf("Reallocated %ld\n", (long)size);
    }*/
    if (MINIMUM_MEMORY_SIZE_FAILURE <= size && size <= MAXIMUM_MEMORY_SIZE_FAILURE) {
        /*free(mem);*/ /* Realloc doesn't free on failure. */
        return NULL;
    }
    return realloc(mem, size);
}
static void U_CALLCONV ctest_libFree(const void *context, void *mem) {
    (void)context; // suppress compiler warnings about unused variable
    free(mem);
}

int T_CTEST_EXPORT2
initArgs( int argc, const char* const argv[], ArgHandlerPtr argHandler, void *context)
{
    int                i;
    int                argSkip = 0;

    VERBOSITY = FALSE;
    ERR_MSG = TRUE;

    ARGV_0=argv[0];

    for( i=1; i<argc; i++)
    {
        if ( argv[i][0] == '/' )
        {
            /* We don't run the tests here. */
            continue;
        }
        else if ((strcmp( argv[i], "-a") == 0) || (strcmp(argv[i],"-all") == 0))
        {
            /* We don't run the tests here. */
            continue;
        }
        else if (strcmp( argv[i], "-v" )==0 || strcmp( argv[i], "-verbose")==0)
        {
            VERBOSITY = TRUE;
        }
        else if (strcmp( argv[i], "-l" )==0 )
        {
            /* doList = TRUE; */
        }
        else if (strcmp( argv[i], "-e1") == 0)
        {
            QUICK = -1;
        }
        else if (strcmp( argv[i], "-e") ==0)
        {
            QUICK = 0;
        }
        else if (strcmp( argv[i], "-K") ==0)
        {
            NO_KNOWN = 1;
        }
        else if (strncmp( argv[i], "-E",2) ==0)
        {
	    SUMMARY_FILE=argv[i]+2;
        }
        else if (strcmp( argv[i], "-w") ==0)
        {
            WARN_ON_MISSING_DATA = TRUE;
        }
        else if (strcmp( argv[i], "-m") ==0)
        {
            UErrorCode errorCode = U_ZERO_ERROR;
            if (i+1 < argc) {
                char *endPtr = NULL;
                i++;
                MINIMUM_MEMORY_SIZE_FAILURE = (size_t)strtol(argv[i], &endPtr, 10);
                if (endPtr == argv[i]) {
                    printf("Can't parse %s\n", argv[i]);
                    help(argv[0]);
                    return 0;
                }
                if (*endPtr == '-') {
                    char *maxPtr = endPtr+1;
                    endPtr = NULL;
                    MAXIMUM_MEMORY_SIZE_FAILURE = (size_t)strtol(maxPtr, &endPtr, 10);
                    if (endPtr == argv[i]) {
                        printf("Can't parse %s\n", argv[i]);
                        help(argv[0]);
                        return 0;
                    }
                }
            }
            /* Use the default value */
            u_setMemoryFunctions(NULL, ctest_libMalloc, ctest_libRealloc, ctest_libFree, &errorCode);
            if (U_FAILURE(errorCode)) {
                printf("u_setMemoryFunctions returned %s\n", u_errorName(errorCode));
                return 0;
            }
        }
        else if(strcmp( argv[i], "-n") == 0 || strcmp( argv[i], "-no_err_msg") == 0)
        {
            ERR_MSG = FALSE;
        }
        else if (strcmp( argv[i], "-r") == 0)
        {
            if (!REPEAT_TESTS_INIT) {
                REPEAT_TESTS++;
            }
        }
        else if (strcmp( argv[i], "-x") == 0)
        {
          if(++i>=argc) {
            printf("* Error: '-x' option requires an argument. usage: '-x outfile.xml'.\n");
            return 0;
          }
          if(ctest_xml_setFileName(argv[i])) { /* set the name */
            return 0;
          }
        }
        else if (strcmp( argv[i], "-t_info") == 0) {
            ICU_TRACE = UTRACE_INFO;
        }
        else if (strcmp( argv[i], "-t_error") == 0) {
            ICU_TRACE = UTRACE_ERROR;
        }
        else if (strcmp( argv[i], "-t_warn") == 0) {
            ICU_TRACE = UTRACE_WARNING;
        }
        else if (strcmp( argv[i], "-t_verbose") == 0) {
            ICU_TRACE = UTRACE_VERBOSE;
        }
        else if (strcmp( argv[i], "-t_oc") == 0) {
            ICU_TRACE = UTRACE_OPEN_CLOSE;
        }
        else if (strcmp( argv[i], "-h" )==0 || strcmp( argv[i], "--help" )==0)
        {
            help( argv[0] );
            return 0;
        }
        else if (argHandler != NULL && (argSkip = argHandler(i, argc, argv, context)) > 0)
        {
            i += argSkip - 1;
        }
        else
        {
            printf("* unknown option: %s\n", argv[i]);
            help( argv[0] );
            return 0;
        }
    }
    if (ICU_TRACE != UTRACE_OFF) {
        utrace_setFunctions(NULL, TraceEntry, TraceExit, TraceData);
        utrace_setLevel(ICU_TRACE);
    }

    return 1; /* total error count */
}

int T_CTEST_EXPORT2
runTestRequest(const TestNode* root,
             int argc,
             const char* const argv[])
{
    /**
     * This main will parse the l, v, h, n, and path arguments
     */
    const TestNode*    toRun;
    int                i;
    int                doList = FALSE;
    int                subtreeOptionSeen = FALSE;

    int                errorCount = 0;

    toRun = root;

    if(ctest_xml_init(ARGV_0)) {
      return 1; /* couldn't fire up XML thing */
    }

    for( i=1; i<argc; i++)
    {
        if ( argv[i][0] == '/' )
        {
            printf("Selecting subtree '%s'\n", argv[i]);

            if ( argv[i][1] == 0 )
                toRun = root;
            else
                toRun = getTest(root, argv[i]);

            if ( toRun == NULL )
            {
                printf("* Could not find any matching subtree\n");
                return -1;
            }

            ON_LINE=FALSE; /* just in case */

            if( doList == TRUE)
                showTests(toRun);
            else
                runTests(toRun);

            ON_LINE=FALSE; /* just in case */

            errorCount += ERROR_COUNT;

            subtreeOptionSeen = TRUE;
        } else if ((strcmp( argv[i], "-a") == 0) || (strcmp(argv[i],"-all") == 0)) {
            subtreeOptionSeen=FALSE;
        } else if (strcmp( argv[i], "-l") == 0) {
            doList = TRUE;
        }
        /* else option already handled by initArgs */
    }

    if( subtreeOptionSeen == FALSE) /* no other subtree given, run the default */
    {
        ON_LINE=FALSE; /* just in case */
        if( doList == TRUE)
            showTests(toRun);
        else
            runTests(toRun);
        ON_LINE=FALSE; /* just in case */

        errorCount += ERROR_COUNT;
    }
    else
    {
        if( ( doList == FALSE ) && ( errorCount > 0 ) )
            printf(" Total errors: %d\n", errorCount );
    }

    REPEAT_TESTS_INIT = 1;
    
    if(ctest_xml_fini()) {
      errorCount++;
    }

    return errorCount; /* total error count */
}

/**
 * Display program invocation arguments
 */

static void help ( const char *argv0 )
{
    printf("Usage: %s [ -l ] [ -v ] [ -verbose] [-a] [ -all] [-n] [ -no_err_msg]\n"
           "    [ -h ] [-t_info | -t_error | -t_warn | -t_oc | -t_verbose] [-m n[-q] ]\n"
           "    [ /path/to/test ]\n",
            argv0);
    printf("    -l  To get a list of test names\n");
    printf("    -e  to do exhaustive testing\n");
    printf("    -verbose To turn ON verbosity\n");
    printf("    -v  To turn ON verbosity(same as -verbose)\n");
    printf("    -x file.xml   Write junit format output to file.xml\n");
    printf("    -h  To print this message\n");
    printf("    -K  to turn OFF suppressing known issues\n");
    printf("    -n  To turn OFF printing error messages\n");
    printf("    -w  Don't fail on data-loading errs, just warn. Useful if\n"
           "        user has reduced/changed the common set of ICU data \n");
    printf("    -t_info | -t_error | -t_warn | -t_oc | -t_verbose  Enable ICU tracing\n");
    printf("    -no_err_msg (same as -n) \n");
    printf("    -m n[-q] Min-Max memory size that will cause an allocation failure.\n");
    printf("        The default is the maximum value of size_t. Max is optional.\n");
    printf("    -r  Repeat tests after calling u_cleanup \n");
    printf("    [/subtest]  To run a subtest \n");
    printf("    eg: to run just the utility tests type: cintltest /tsutil) \n");
}

int32_t T_CTEST_EXPORT2
getTestOption ( int32_t testOption ) {
    switch (testOption) {
        case VERBOSITY_OPTION:
            return VERBOSITY;
        case WARN_ON_MISSING_DATA_OPTION:
            return WARN_ON_MISSING_DATA;
        case QUICK_OPTION:
            return QUICK;
        case REPEAT_TESTS_OPTION:
            return REPEAT_TESTS;
        case ERR_MSG_OPTION:
            return ERR_MSG;
        case ICU_TRACE_OPTION:
            return ICU_TRACE;
        default :
            return 0;
    }
}

void T_CTEST_EXPORT2
setTestOption ( int32_t testOption, int32_t value) {
    if (value == DECREMENT_OPTION_VALUE) {
        value = getTestOption(testOption);
        --value;
    }
    switch (testOption) {
        case VERBOSITY_OPTION:
            VERBOSITY = value;
            break;
        case WARN_ON_MISSING_DATA_OPTION:
            WARN_ON_MISSING_DATA = value;
            break;
        case QUICK_OPTION:
            QUICK = value;
            break;
        case REPEAT_TESTS_OPTION:
            REPEAT_TESTS = value;
            break;
        case ICU_TRACE_OPTION:
            ICU_TRACE = (UTraceLevel)value;
            break;
        default :
            break;
    }
}


/*
 * ================== JUnit support ================================
 */

int32_t
T_CTEST_EXPORT2
ctest_xml_setFileName(const char *name) {
  XML_FILE_NAME=name;
  return 0;
}


int32_t
T_CTEST_EXPORT2
ctest_xml_init(const char *rootName) {
  if(!XML_FILE_NAME) return 0;
  XML_FILE = fopen(XML_FILE_NAME,"w");
  if(!XML_FILE) {
    perror("fopen");
    fprintf(stderr," Error: couldn't open XML output file %s\n", XML_FILE_NAME);
    return 1;
  }
  while(*rootName&&!isalnum((int)*rootName)) {
    rootName++;
  }
  strcpy(XML_PREFIX,rootName);
  {
    char *p = XML_PREFIX+strlen(XML_PREFIX);
    for(p--;*p&&p>XML_PREFIX&&!isalnum((int)*p);p--) {
      *p=0;
    }
  }
  /* write prefix */
  fprintf(XML_FILE, "<testsuite name=\"%s\">\n", XML_PREFIX);

  return 0;
}

int32_t
T_CTEST_EXPORT2
ctest_xml_fini(void) {
  if(!XML_FILE) return 0;

  fprintf(XML_FILE, "</testsuite>\n");
  fclose(XML_FILE);
  printf(" ( test results written to %s )\n", XML_FILE_NAME);
  XML_FILE=0;
  return 0;
}


int32_t
T_CTEST_EXPORT2
ctest_xml_testcase(const char *classname, const char *name, const char *timeSeconds, const char *failMsg) {
  if(!XML_FILE) return 0;

  fprintf(XML_FILE, "\t<testcase classname=\"%s:%s\" name=\"%s:%s\" time=\"%s\"", XML_PREFIX, classname, XML_PREFIX, name, timeSeconds);
  if(failMsg) {
    fprintf(XML_FILE, ">\n\t\t<failure type=\"err\" message=\"%s\"/>\n\t</testcase>\n", failMsg);
  } else {
    fprintf(XML_FILE, "/>\n");
  }

  return 0;
}


