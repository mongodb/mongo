/*************************************************
*        PCRE2 POSIX interface test program      *
*************************************************/

/*
Written by Philip Hazel, December 2022
Copyright (c) 2022
File last edited: December 2022

This program tests the POSIX wrapper to the PCRE2 regular expression library.
The main PCRE2 test program is pcre2test, which also tests these function
calls. This little program is needed to test the case where the client includes
pcre2posix.h but not pcre2.h, mainly to make sure that it builds successfully.
However, the code is written as a flexible test program to which extra tests
can be added.

Compile with -lpcre2-posix -lpcre2-8

If run with no options, there is no output on success, and the return code is
zero. If any test fails there is output to stderr, and the return code is 1.

For testing purposes, the "-v" option causes verification output to be written
to stdout. */

#include <stdio.h>
#include <string.h>
#include <pcre2posix.h>

#define CAPCOUNT 5               /* Number of captures supported */
#define PRINTF if (v) printf     /* Shorthand for testing output */

/* This vector contains compiler flags for each pattern that is tested. */

static int cflags[] = {
  0,           /* Test 0 */
  REG_ICASE,   /* Test 1 */
  0,           /* Test 2 */
  REG_NEWLINE, /* Test 3 */
  0            /* Test 4 */
};

/* This vector contains match flags for each pattern that is tested. */

static int mflags[] = {
  0,           /* Test 0 */
  0,           /* Test 1 */
  0,           /* Test 2 */
  REG_NOTBOL,  /* Test 3 */
  0            /* Test 4 */
};

/* Automate the number of patterns */

#define count (int)(sizeof(cflags)/sizeof(int))

/* The data for each pattern consists of a pattern string, followed by any
number of subject strings, terminated by NULL. Some tests share data, but use
different flags. */

static const char *data0_1[] = { "posix", "lower posix", "upper POSIX", NULL };
static const char *data2_3[] = { "^(cat|dog)", "catastrophic\ncataclysm",
  "dogfight", "no animals", NULL };
static const char *data4[] = { "*badpattern", NULL };

/* Index the data strings */

static char **data[] = {
  (char **)(&data0_1),
  (char **)(&data0_1),
  (char **)(&data2_3),
  (char **)(&data2_3),
  (char **)(&data4)
};

/* The expected results for each pattern consist of a compiler return code,
optionally followed, for each subject string, by a match return code and, for a
successful match, up to CAPCOUNT pairs of returned match data. */

static int results0[] = {
  0,             /* Compiler rc */
  0, 6, 11,      /* 1st match */
  REG_NOMATCH    /* 2nd match */
};

static int results1[] = {
  0,             /* Compiler rc */
  0, 6, 11,      /* 1st match */
  0, 6, 11       /* 2nd match */
};

static int results2[] = {
  0,             /* Compiler rc */
  0, 0, 3, 0, 3, /* 1st match */
  0, 0, 3, 0, 3, /* 2nd match */
  REG_NOMATCH    /* 3rd match */
};

static int results3[] = {
  0,                 /* Compiler rc */
  0, 13, 16, 13, 16, /* 1st match */
  REG_NOMATCH,       /* 2nd match */
  REG_NOMATCH        /* 3rd match */
};

static int results4[] = {
  REG_BADRPT         /* Compiler rc */
};

/* Index the result vectors */

static int *results[] = {
  (int *)(&results0),
  (int *)(&results1),
  (int *)(&results2),
  (int *)(&results3),
  (int *)(&results4)
};

/* And here is the program */

int main(int argc, char **argv)
{
regex_t re;
regmatch_t match[CAPCOUNT];
int v = argc > 1 && strcmp(argv[1], "-v") == 0;

PRINTF("Test of pcre2posix.h without pcre2.h\n");

for (int i = 0; i < count; i++)
  {
  char *pattern = data[i][0];
  char **subjects = data[i] + 1;
  int *rd = results[i];
  int rc = regcomp(&re, pattern, cflags[i]);

  PRINTF("Pattern: %s flags=0x%02x\n", pattern, cflags[i]);

  if (rc != *rd)
    {
    fprintf(stderr, "Unexpected compile error %d (expected %d)\n", rc, *rd);
    fprintf(stderr, "Pattern is: %s\n", pattern);
    return 1;
    }

  if (rc != 0)
    {
    if (v)
      {
      char buffer[256];
      (void)regerror(rc, &re, buffer, sizeof(buffer));
      PRINTF("Compile error %d: %s (expected)\n", rc, buffer);
      }
    continue;
    }

  for (; *subjects != NULL; subjects++)
    {
    rc = regexec(&re, *subjects, CAPCOUNT, match, mflags[i]);

    PRINTF("Subject: %s\n", *subjects);
    PRINTF("Return:  %d", rc);

    if (rc != *(++rd))
      {
      PRINTF("\n");
      fprintf(stderr, "Unexpected match error %d (expected %d)\n", rc, *rd);
      fprintf(stderr, "Pattern is: %s\n", pattern);
      fprintf(stderr, "Subject is: %s\n", *subjects);
      return 1;
      }

    if (rc == 0)
      {
      for (int j = 0; j < CAPCOUNT; j++)
        {
        regmatch_t *m = match + j;
        if (m->rm_so < 0) continue;
        if (m->rm_so != *(++rd) || m->rm_eo != *(++rd))
          {
          PRINTF("\n");
          fprintf(stderr, "Mismatched results for successful match\n");
          fprintf(stderr, "Pattern is: %s\n", pattern);
          fprintf(stderr, "Subject is: %s\n", *subjects);
          fprintf(stderr, "Result %d: expected %d %d received %d %d\n",
            j, rd[-1], rd[0], m->rm_so, m->rm_eo);
          return 1;
          }
        PRINTF(" (%d %d %d)", j, m->rm_so, m->rm_eo);
        }
      }

    else if (v)
      {
      char buffer[256];
      (void)regerror(rc, &re, buffer, sizeof(buffer));
      PRINTF(": %s (expected)", buffer);
      }

    PRINTF("\n");
    }

  regfree(&re);
  }

PRINTF("End of test\n");
return 0;
}

/* End of pcre2posix_test.c */
