/*
Copyright (c) 2007-2011, Intel Corp.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// readtest.c - read tests from stdin
//
// This programs reads test of the form:
//
// <TESTID> <FUNCTION> <OP1> <OP2> <OP3> <RESULT> <STATUS>
//
// The testID is simply a string used to help identify which tail may be failing.
// The function name is generally one of the BID library function names.  Up to 3
// operands follow the function name, and then the expected result and expected 
// status value.
//
// Each test is read, the appropriate function is called, and the results are 
// compared with the expected results.  The operands, and results can appear as
// decimal numbers (e.g. 6.25), or as hexadecimal representations surrounded by
// square brackets (e.g. [31c0000000012345]).  The status value is a hexadecimal
// value (without the leading 0x).

#define STRMAX	1024
#include <stdio.h>	// for printf
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#if !defined _MSC_VER && !defined __INTEL_COMPILER
#include <fenv.h>	
#endif

#include "test_bid_conf.h"
#include "test_bid_functions.h"

int copy_str_to_wstr();

enum _OPTYPE { OP_NONE, OP_DEC128, OP_DPD128, OP_DEC64, OP_DPD64,
    OP_DEC32, OP_DPD32,
  OP_INT64, OP_BID_UINT64, OP_INT32, OP_BID_UINT32, OP_BIN64, OP_BIN32,
    OP_BIN80, OP_BIN128,
  OP_INT16, OP_BID_UINT16, OP_INT8, OP_BID_UINT8, OP_LINT, OP_STRING
};

// Compare modes: EXACT does a bitwise compare, FUZZY does a bitwise unless NaN vs. NaN
//                EQUAL uses bid<size>_quiet_equal

enum _CMPTYPE { CMP_NONE, CMP_EXACT, CMP_FUZZY,
  CMP_EXACTSTATUS, CMP_FUZZYSTATUS,
  CMP_EQUAL, CMP_EQUALSTATUS, CMP_RELATIVEERR
};

typedef BID_UINT32 decimal32;
typedef BID_UINT64 decimal64;
typedef BID_UINT128 decimal128;

#define GETTEST1(res,op1) restype = (res); op1type = (op1); get_test();
#define GETTEST2(res,op1,op2) restype = (res); op1type = (op1); op2type = (op2); get_test();
#define GETTEST3(res,op1,op2,op3) restype = (res); op1type = (op1); op2type = (op2); op3type = (op3); get_test();

BID_EXTERN_C char *decStatusToString (const int status);
BID_EXTERN_C int check128 (BID_UINT128 a, BID_UINT128 b);
BID_EXTERN_C int check64 (BID_UINT64 a, BID_UINT64 b);
BID_EXTERN_C int compare_status (BID_FPSC bid, int dpd);
BID_EXTERN_C void print_status (int st);
BID_EXTERN_C void get_status (int st, char *status_str);
BID_EXTERN_C char *func;
BID_EXTERN_C double fabs(double);
double ulp; // current integer difference computed_result-expected_result
double ulp_add;
int Underflow_Before;
int li_size_test, li_size_run;
double mre;
float snan_check32;
double snan_check64;
long double snan_check80;
double mre_max[5] = {2.0, 2.0, 2.0, 2.0, 2.0};
unsigned int  trans_flags_mask = 0x05;
int not_special_arg_res;
int SNaN_passed_incorrectly32 = 0;
int SNaN_passed_incorrectly64 = 0;
int SNaN_passed_incorrectly80 = 0;
int Den_passed_incorrectly32 = 0;
int Den_passed_incorrectly64 = 0;
int Den_passed_incorrectly80 = 0;
int arg32_den, res32_den, arg32_snan;
int arg64_den, res64_den, arg64_snan;
int arg80_den, res80_den, arg80_snan;
int res128_den;
int pollution_workaround;

fexcept_t ini_binary_flags, saved_binary_flags, test_binary_flags;
int ini_binary_rmode, saved_binary_rmode, test_binary_rmode;
fexcept_t fp_fl;

void save_binary_status();
int check_restore_binary_status();
int check_pollution_workaround(void);

#ifndef HPUX_OS
#ifndef FE_UNNORMAL
#define FE_UNNORMAL     2
#endif
#ifndef FE_DENORMAL
#define FE_DENORMAL     2
#endif
#else
#ifndef FE_UNNORMAL
#define FE_UNNORMAL     0x4000
#endif
#ifndef FE_DENORMAL
#define FE_DENORMAL     0x4000
#endif
#endif

#define getop128(bid, dpd, op, str) \
      if (*op == '[') { \
        if ((sscanf(op+1, BID_FMT_LLX16""BID_FMT_LLX16, &(bid.w[BID_HIGH_128W]), &(bid.w[BID_LOW_128W])) == 2) || \
            (sscanf(op+1, ""BID_FMT_LLX16","BID_FMT_LLX16"", &(bid.w[BID_HIGH_128W]), &(bid.w[BID_LOW_128W])) == 2)) {\
          dpd = bid; \
          BIDECIMAL_CALL1_NORND_RESREF(bid128_to_string, str, bid); \
		  } else { \
			 printf("Internal error - can't read number form string %s\n", op+1); \
			 exit(1); \
		  } \
      } else { \
        BIDECIMAL_CALL1_RESARG(bid128_from_string, (bid), (op)); \
        dpd = bid; \
        if ( !strcmp(func, "bid128_from_string") || !strcmp(func, "bid_strtod128") || !strcmp(func, "bid_wcstod128") || !strcmp(func, "bid_strtod128") || !strcmp(func, "bid128_nan") ) { \
            strcpy(str, op); \
        } else { \
            BIDECIMAL_CALL1_NORND_RESREF(bid128_to_string, str, bid); \
        } \
      } \

#define getop32(bid, dpd, op, str) \
      if (*op == '[') { \
        if (sscanf((op)+1, BID_FMT_X8, &(bid))) { \
          dpd = (bid); \
          BIDECIMAL_CALL1_NORND_RESREF(bid32_to_string, str, bid); \
		  } else { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        BIDECIMAL_CALL1_RESARG(bid32_from_string, bid, op); \
        dpd = bid; \
        if ( !strcmp(func, "bid32_from_string") || !strcmp(func, "bid_strtod32") ||  !strcmp(func, "bid_wcstod32") || !strcmp(func, "bid32_nan") ) { \
            strcpy(str, op); \
        } else { \
            BIDECIMAL_CALL1_NORND_RESREF(bid32_to_string, str, bid); \
        } \
      } \


#define getop64(bid, dpd, op, str) \
      if (*op == '[') { \
        if (sscanf(op+1, BID_FMT_LLX16, &bid)) { \
          dpd = bid; \
          BIDECIMAL_CALL1_NORND_RESREF(bid64_to_string, str, bid); \
		  } else { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        BIDECIMAL_CALL1_RESARG(bid64_from_string, bid, op); \
        dpd = bid; \
        if ( !strcmp(func, "bid64_from_string") || !strcmp(func, "bid_strtod64") ||  !strcmp(func, "bid_wcstod64") || !strcmp(func, "bid64_nan") ) { \
            strcpy(str, op); \
        } else { \
            BIDECIMAL_CALL1_NORND_RESREF(bid64_to_string, str, bid); \
        } \
      } 

#define getop32i(bid, op, str) \
      if (*op == '[') { \
        if (!sscanf(op+1, "%08x", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf(op, "%d", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", op); \
			 exit(1); \
		  } \
      } \
      sprintf(str, "%d", bid);

#define getop16i(bid, op, str) \
      if (*op == '[') { \
        if (!sscanf(op+1, "%04x", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf(op, "%d", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
      } \
      sprintf(str, "%d", bid);

#define getop8i(bid, op, str) \
      if (*op == '[') { \
        if (!sscanf(op+1, "%02x", &bid)) { \
		  } else { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf(op, "%d", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
      } \
      sprintf(str, "%d", bid);

#define getop32u(bid, op, str) \
      if (*op == '[') { \
        if (!sscanf(op+1, "%08x", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf(op, "%u", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
      } \
      sprintf(str, "%u", bid);

#define getop16u(bid, op, str) \
      if (*op == '[') { \
        if (!sscanf(op+1, "%04x", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf(op, "%u", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
      } \
      sprintf(str, "%u", bid);

#define getop8u(bid, op, str) \
      if (*op == '[') { \
        if (!sscanf(op+1, "%02x", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf(op, "%u", &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
      } \
      sprintf(str, "%u", bid);

#define getop64i(bid, op, str) \
      if (*op == '[') { \
        if (!sscanf(op+1, BID_FMT_LLX16, &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf(op, BID_FMT_LLD, &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
      } \
      sprintf(str, BID_FMT_LLD, bid);

#define getop64u(bid, op, str) \
      if (*op == '[') { \
        if (!sscanf(op+1, BID_FMT_LLX16, &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf(op, BID_FMT_LLU, &bid)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
      } \
      sprintf(str, BID_FMT_LLU, bid);

#define getopquad(quad1, quad2, op, str) \
      if (*op == '[') { \
        if (sscanf(op+1, BID_FMT_LLX16""BID_FMT_LLX16, ((BID_UINT64*)&quad1+BID_HIGH_128W), (BID_UINT64*)&quad1+BID_LOW_128W) != 2) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } \
     strcpy(str, "unavalable");

#if BID_BIG_ENDIAN
#define getopldbl(ldbl1, ldbl2, op, str) \
{ \
int tmpi; \
		arg80_den = arg80_snan = 0; \
      if (*op == '[') { \
        if (sscanf(op+1, BID_FMT_LLX16""BID_FMT_X4, (BID_UINT64*)&ldbl1, &tmpi) != 2) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } else {\
          *((unsigned short*)((BID_UINT64*)&ldbl1+1)) = tmpi & 0xffff; \
        } \
      } else { \
        double dtmp; \
        if (!sscanf (op, "%lf", &dtmp)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
		  ldbl1 = (BINARY80)dtmp; \
      } \
		{ short *px = (short*)&ldbl1; \
			if (((*(px) & 0x7fff) == 0x7fff) && ((*(px+1) & 0x8000) == 0x8000) && ((*(px+1) & 0x3fff) || (*(px+2) & 0xffff) || (*(px+3) & 0xffff) || (*(px+0) & 0xffff)) && !(*(px+1) & 0x4000)) arg80_snan = 1; \
			if (((*(px) & 0x7fff) == 0x0000) && ((*(px+1) & 0x8000) != 0x8000) && ((*(px+1) & 0x3fff) || (*(px+2) & 0xffff) || (*(px+3) & 0xffff) || (*(px+0) & 0xffff))) arg80_den = 1; \
		} \
     sprintf(str, "%27.17e", (double)ldbl1); \
}
#else
#define getopldbl(ldbl1, ldbl2, op, str) \
		arg80_den = arg80_snan = 0; \
      if (*op == '[') { \
        if (sscanf(op+1, BID_FMT_X4""BID_FMT_LLX16, (unsigned short*)((BID_UINT64*)&ldbl1+1), (BID_UINT64*)&ldbl1) != 2) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        double dtmp; \
        if (!sscanf (op, "%lf", &dtmp)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
		  ldbl1 = (BINARY80)dtmp; \
      } \
		{ short *px = (short*)&ldbl1; \
			if (((*(px+4) & 0x7fff) == 0x7fff) && ((*(px+3) & 0x8000) == 0x8000) && ((*(px+3) & 0x3fff) || (*(px+2) & 0xffff) || (*(px+1) & 0xffff) || (*(px+0) & 0xffff)) && !(*(px+3) & 0x4000)) arg80_snan = 1; \
			if (((*(px+4) & 0x7fff) == 0x0000) && ((*(px+3) & 0x8000) != 0x8000) && ((*(px+3) & 0x3fff) || (*(px+2) & 0xffff) || (*(px+1) & 0xffff) || (*(px+0) & 0xffff))) arg80_den = 1; \
		} \
     sprintf(str, "%27.17e", (double)ldbl1); 
#endif

#define getopdbl(dbl1, dbl2, op, str) \
		arg64_den = arg64_snan = 0; \
      if (*op == '[') { \
        if (!sscanf(op+1, BID_FMT_LLX16, (BID_UINT64*)&dbl1)) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf (op, "%lf", &dbl1)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
      } \
		{ int *px = (int*)&dbl1; \
			if (((*(px+BID_HIGH_128W) & 0x7ff00000) == 0x7ff00000) && ((*(px+BID_HIGH_128W) & 0x0007ffff) || (*(px+BID_LOW_128W) & 0xffffffff)) && !(*(px+BID_HIGH_128W) & 0x00080000)) arg64_snan = 1; \
			if (((*(px+BID_HIGH_128W) & 0x7ff00000) == 0x00000000) && ((*(px+BID_HIGH_128W) & 0x000fffff) || (*(px+BID_LOW_128W) & 0xffffffff))) arg64_den = 1; \
		} \
     sprintf(str, "%27.17e", dbl1);

#define getopflt(flt1, flt2, op, str) \
		arg32_den = arg32_snan = 0; \
      if (*op == '[') { \
        if (!sscanf(op+1, BID_FMT_X8, (BID_UINT32*)&flt1)) { \
			 printf("Internal error - can't read number form string %s\n", (op)+1); \
			 exit(1); \
		  } \
      } else { \
        if (!sscanf (op, "%f", &flt1)) { \
			 printf("Internal error - can't read number form string %s\n", (op)); \
			 exit(1); \
		  } \
      } \
		{ int *px = (int*)&flt1;  \
			if (((*(px) & 0x7f800000) == 0x7f800000) && (*(px) & 0x003fffff) && !(*(px) & 0x00400000)) arg32_snan = 1; \
			if (((*(px) & 0x7f800000) == 0x00000000) && (*(px) & 0x007fffff)) arg32_den = 1; \
		} \
     sprintf(str, "%18.9e", flt1);

BID_EXTERN_C void get_ops (void);
BID_EXTERN_C void get_test (void);
BID_EXTERN_C void gen_ops (void);
BID_EXTERN_C void print_mismatch (enum _CMPTYPE cmp);
BID_EXTERN_C void check_results (enum _CMPTYPE cmp);
BID_EXTERN_C int st_compare (char **a, char **b);
BID_EXTERN_C int status_compare (char *stat1, char *stat2);
BID_EXTERN_C int setrounding (char *s);

unsigned int fpsf_0 = 0;

int args_set;

#if !DECIMAL_GLOBAL_EXCEPTION_FLAGS
unsigned int pfpsf_value;
unsigned int *pfpsf = &pfpsf_value;
#endif

#if defined LINUX && !defined HPUX_OS
#include <getopt.h>
#endif

#ifndef __COMPAR_FN_T
# define __COMPAR_FN_T
#if !defined(WINDOWS)
typedef int (*__compar_fn_t) (__const void *, __const void *);
#endif
#endif

#define STRMAX	1024

enum _OPTYPE restype, op1type, op2type, op3type;

BID_UINT128 A, B, C, T, Q, R, R_1;
BID_UINT64 A64, B64, C64, CC64, T64, Q64, R64, Rt64, R64_1;
BID_UINT32 A32, B32, C32, T32, Q32, R32, Rt32, R32_1;
BINARY128 Aquad, Rquad, Rtquad;
BINARY80 Aldbl, Rldbl, Rtldbl;
double Adbl, Rdbl, Rtdbl;
float Aflt, Rflt, Rtflt;

BID_UINT32 a32, b32, c32, q32, r32;
BID_UINT64 a64, b64, c64, q64, r64;
BID_UINT128 a, b, c, q, r;

char AI8;
unsigned char AUI8;
short AI16, BI16;
unsigned short AUI16, BUI16;
int AI32, BI32;
unsigned int AUI32, BUI32, CUI32;
BID_SINT64 AI64;
BID_UINT64 AUI64;
long int BLI, li1, li2;

unsigned int u1, u2;
int i1, i2, n;

unsigned short u1_16, u2_16;
short i1_16, i2_16;

unsigned char u1_8, u2_8;
char i1_8, i2_8;

unsigned int expected_status;

#if !DECIMAL_GLOBAL_ROUNDING
unsigned int rnd_mode;
#endif
unsigned int tmp_status;
unsigned int rnd = 0;

char funcstr[STRMAX];
char *func = funcstr;
char string[STRMAX];		// conversion buffer
char str1[STRMAX];

// Input and result operands
char op1[STRMAX], op2[STRMAX], op3[STRMAX];	// conversion strings
char res[STRMAX];

char *endptr;
wchar_t wistr1[STRMAX], *wendptr;
char str_prefix[STRMAX];
char istr1[STRMAX], istr2[STRMAX], istr3[STRMAX], str2[STRMAX];
char convstr[STRMAX];
char res[STRMAX], rstr[STRMAX];
char version[STRMAX];
int extended, precision, minExponent, maxExponent;
char rounding[STRMAX];
int line_counter;
char result[STRMAX];
char status_str[STRMAX];
char exp_result[STRMAX], exp_status[STRMAX];
char line[STRMAX];
char full_line[STRMAX];
char *p;
BID_UINT64 Qi64, qi64;
int specop_opt;
int randop_opt;
int answer_opt;
int canon_opt;
int canon_dpd_opt;
int prec;
int expon_min;
int expon_max;
int no128trans = 0;
int no64trans = 0;


int debug_opt = 0;
#if defined UNCHANGED_BINARY_STATUS_FLAGS
int check_binary_flags_opt = 1;
#else
int check_binary_flags_opt = 0;
#endif
int underflow_before_opt = 0;
int underflow_after_opt = 0;
int tests = 0, fail_res = 0, fail_status = 0;

// char * decimal32ToString(const decimal32 *, char *);
// char * decimal64ToString(const decimal64 *, char *);
// char * decimal128ToString(const decimal128 *, char *);

char *roundstr[] = { "even", "away", "up", "down", "zero" };
char *roundstr_bid[] =
  { "half_even", "down", "up", "zero", "half_away" };

int
setrounding (char *s) {
  if (strcmp (s, "half_even") == 0 || strcmp (s, "even") == 0) {
    rnd = rnd_mode = BID_ROUNDING_TO_NEAREST;
    strcpy (rounding, roundstr[0]);
  } else if (strcmp (s, "half_away") == 0 || strcmp (s, "away") == 0) {
    rnd = rnd_mode = BID_ROUNDING_TIES_AWAY;
    strcpy (rounding, roundstr[1]);
  } else if (strcmp (s, "up") == 0) {
    rnd = rnd_mode = BID_ROUNDING_UP;
    strcpy (rounding, roundstr[2]);
  } else if (strcmp (s, "down") == 0) {
    rnd = rnd_mode = BID_ROUNDING_DOWN;
    strcpy (rounding, roundstr[3]);
  } else if (strcmp (s, "zero") == 0) {
    rnd = rnd_mode = BID_ROUNDING_TO_ZERO;
    strcpy (rounding, roundstr[4]);
  } else {
    printf
      ("setrounding: unknown rounding mode string!!!  Mode unchanged.\n");
  }
  return 1;
}

// Return TRUE (1) if 'a' and 'b' are incomtible results from some BID and DPD
// functions.  Ignore the payload of NaNs
int
check128 (BID_UINT128 a, BID_UINT128 b) {
  if (a.w[BID_LOW_128W] == b.w[BID_LOW_128W]
      && a.w[BID_HIGH_128W] == b.w[BID_HIGH_128W])
    return 0;

  return 1;
}

// Get rid of 0x0D and 0x0A symbols in the string s.
void strRemove0D0A(char* s) {
    int i = 0;
    while( (s[i]!=0) && (s[i]!=0x0D) && (s[i]!=0x0A) ) i++;
    s[i]=0;
}

// Get rid of trailing spaces in the string s.
void strRemoveTrailingSpaces(char* s) {
    int i=0;
    while( s[i]!=0 ) i++; i--;
    while( (s[i]==' ') && (i>=0) ) { s[i]=0; i--; }
}

int
check64 (BID_UINT64 a, BID_UINT64 b) {
  if (a == b)
    return 0;

  return 1;
}

int
check32 (BID_UINT32 a32, BID_UINT32 b32) {
  if (a32 == b32)
    return 0;

  return 1;
}

#define MASK_STEERING_BITS              0x6000000000000000ull
#define MASK_EXP                        0x7ffe000000000000ull
#define EXP_P1                          0x0002000000000000ull
#define SMALL_COEFF_MASK128             0x0001ffffffffffffull
#define MASK_BINARY_EXPONENT2           0x1ff8000000000000ull
#define MASK_BINARY_EXPONENT1           0x7fe0000000000000ull
#define MASK_BINARY_SIG2                0x0007ffffffffffffull
#define MASK_BINARY_SIG1                0x001fffffffffffffull
#define MASK_BINARY_OR2                 0x0020000000000000ull
#define MASK_STEERING_BITS32            0x60000000
#define MASK_BINARY_EXPONENT1_32        0x7f800000
#define MASK_BINARY_SIG1_32             0x007fffff
#define MASK_BINARY_EXPONENT2_32        0x1fe00000
#define MASK_BINARY_SIG2_32             0x001fffff
#define MASK_BINARY_OR2_32              0x00800000
#define SHIFT_EXP1_32 23
#define SHIFT_EXP2_32 21
#define SHIFT_EXP1_64 53
#define SHIFT_EXP2_64 51
#define SHIFT_EXP1_128 17
#define SHIFT_EXP2_128 15


#define GET_EXP_32(r, x) \
	{ \
		if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) { \
			r =  (x & MASK_BINARY_EXPONENT2_32) >> SHIFT_EXP2_32; \
		} else { \
			r =  (x & MASK_BINARY_EXPONENT1_32) >> SHIFT_EXP1_32; \
		} \
	}

#define GET_MANT_32(r, x) \
	{ \
		if ((x & MASK_STEERING_BITS32) == MASK_STEERING_BITS32) { \
			r =  (x & MASK_BINARY_SIG2_32) | MASK_BINARY_OR2_32; \
		} else { \
			r =  (x & MASK_BINARY_SIG1_32); \
		} \
	}

#define GET_EXP_64(r, x) \
	{ \
		if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) { \
			r =  (x & MASK_BINARY_EXPONENT2) >> SHIFT_EXP2_64; \
		} else { \
			r =  (x & MASK_BINARY_EXPONENT1) >> SHIFT_EXP1_64; \
		} \
	}

#define GET_MANT_64(r, x) \
	{ \
		if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) { \
			r =  (x & MASK_BINARY_SIG2) | MASK_BINARY_OR2; \
		} else { \
			r =  (x & MASK_BINARY_SIG1); \
		} \
	}

#define MASK_EXP2 0x1fff800000000000ull
#define MASK_SIG2 0x00007fffffffffffull

#define GET_EXP_128(r, x) \
	{ \
		if ((x & MASK_STEERING_BITS) == MASK_STEERING_BITS) { \
			r =  (x & MASK_EXP2) >> SHIFT_EXP2_128; \
		} else { \
			r =  (x & MASK_EXP) >> SHIFT_EXP1_128; \
		} \
	}

#define GET_MANT_128(r, x) \
	{ \
		if ((x.w[BID_HIGH_128W] & MASK_STEERING_BITS) == MASK_STEERING_BITS) { \
			r.w[BID_HIGH_128W] = (x.w[BID_HIGH_128W] & MASK_SIG2) | EXP_P1; \
			r.w[BID_LOW_128W] = x.w[BID_LOW_128W]; \
		} else { \
			r.w[BID_HIGH_128W] =  (x.w[BID_HIGH_128W] & SMALL_COEFF_MASK128); \
			r.w[BID_LOW_128W] = x.w[BID_LOW_128W]; \
		} \
	}



int
check128_rel(BID_UINT128 a, BID_UINT128 b)
{
	BID_UINT128 r;
	BID_UINT128 r1, r2, m1, m2;
	BID_UINT64 e1, e2;
	int sign, less;
        int t1, t2, t3, t4;

        BIDECIMAL_CALL1_NORND_NOSTAT (bid128_isNaN, t1, a);
        BIDECIMAL_CALL1_NORND_NOSTAT (bid128_isNaN, t2, b);
        BIDECIMAL_CALL1_NORND_NOSTAT (bid128_isInf, t3, a);
        BIDECIMAL_CALL1_NORND_NOSTAT (bid128_isInf, t4, b);

   //if (bid128_isNaN(a)||bid128_isNaN(b) || bid128_isInf(a)||bid128_isInf(b)) {
   if (t1 || t2 || t3 || t4) {
		if (a.w[BID_LOW_128W] == b.w[BID_LOW_128W] && a.w[BID_HIGH_128W] == b.w[BID_HIGH_128W]) return 0;
		else { 
			if(t3) { 
				a.w[BID_HIGH_128W] = (a.w[BID_HIGH_128W] & 0x8000000000000000ull) | 0x5FFFED09BEAD87C0ull;  
				a.w[BID_LOW_128W] = 0x378D8E63FFFFFFFFull;
			} else if(t4) {
				b.w[BID_HIGH_128W] = (b.w[BID_HIGH_128W] & 0x8000000000000000ull) | 0x5FFFED09BEAD87C0ull;  
				b.w[BID_LOW_128W] = 0x378D8E63FFFFFFFFull;
			}
			else return 1;
		}
	}

	if ((a.w[BID_HIGH_128W] & 0x8000000000000000ull) != (b.w[BID_HIGH_128W] & 0x8000000000000000ull)) {
		return 1;
	}

	if (a.w[BID_HIGH_128W] & 0x8000000000000000ull) sign = 1;

	GET_EXP_128(e1, a.w[BID_HIGH_128W])
	GET_EXP_128(e2, b.w[BID_HIGH_128W])
	GET_MANT_128(m1, a)
	GET_MANT_128(m2, b)

	if (e1 < e2) {
		BIDECIMAL_CALL2 (bid128_quantize, r1, a, b);
		r2 = b;
		GET_EXP_128(e1, r1.w[BID_HIGH_128W])
		GET_EXP_128(e2, r2.w[BID_HIGH_128W])
		GET_MANT_128(m1, r1)
		GET_MANT_128(m2, r2)
	} else if (e2 < e1) {
		r1 = a;
		BIDECIMAL_CALL2 (bid128_quantize, r2, b, a);
		GET_EXP_128(e1, r1.w[BID_HIGH_128W])
		GET_EXP_128(e2, r2.w[BID_HIGH_128W])
		GET_MANT_128(m1, r1)
		GET_MANT_128(m2, r2)
	} else {
		r1 = a;
		r2 = b;
	}

	if (e1 != e2) {
		printf("ERROR a, b "BID_FMT_LLX16" "BID_FMT_LLX16" "BID_FMT_LLX16" "BID_FMT_LLX16"\n", a.w[BID_HIGH_128W], a.w[BID_LOW_128W], b.w[BID_HIGH_128W], b.w[BID_LOW_128W]);
		printf("ERROR r1, r2 "BID_FMT_LLX16" "BID_FMT_LLX16" "BID_FMT_LLX16" "BID_FMT_LLX16"\n", r1.w[BID_HIGH_128W], r1.w[BID_LOW_128W], r2.w[BID_HIGH_128W], r2.w[BID_LOW_128W]);
		return 1;
	}

	ulp = m1.w[BID_LOW_128W] > m2.w[BID_LOW_128W] ? 
            m1.w[BID_LOW_128W] - m2.w[BID_LOW_128W] : m2.w[BID_LOW_128W]- m1.w[BID_LOW_128W];
        //TODO HIGH part difference 
   	BIDECIMAL_CALL2_NORND (bid128_quiet_less, less, a, b);
	if (less) ulp *= -1.0;
//printf("ulp %f +add %f max %f\n", ulp, ulp+ulp_add, mre_max[rnd_mode]);
	if (fabs(ulp+ulp_add) > mre_max[rnd_mode]) {
		return 1;
	}

	return 0;
}

int
check64_rel(BID_UINT64 a, BID_UINT64 b)
{
	BID_UINT64 r1, r2;
	BID_UINT64 e1, e2, m1, m2;
	int sign, less;
        int t1, t2, t3, t4;
 
        BIDECIMAL_CALL1_NORND_NOSTAT (bid64_isNaN, t1, a);
        BIDECIMAL_CALL1_NORND_NOSTAT (bid64_isNaN, t2, b);
        BIDECIMAL_CALL1_NORND_NOSTAT (bid64_isInf, t3, a);
        BIDECIMAL_CALL1_NORND_NOSTAT (bid64_isInf, t4, b);
 
   //if (bid64_isNaN(a) || bid64_isNaN(b) || bid64_isInf(a) || bid64_isInf(b)) {
   if (t1 || t2 || t3 || t4) { 
       if (a == b) return 0;
		else return 1;
	}

	if ((a & 0x8000000000000000ull) != (b & 0x8000000000000000ull)) {
		return 1;
	}
	if (a & 0x8000000000000000ull) sign = 1;

	GET_EXP_64(e1, a)
	GET_EXP_64(e2, b)
	GET_MANT_64(m1, a)
	GET_MANT_64(m2, b)

	if (e1 < e2) {
		BIDECIMAL_CALL2 (bid64_quantize, r1, a, b);
		r2 = b;
		GET_EXP_64(e1, r1)
		GET_EXP_64(e2, r2)
		GET_MANT_64(m1, r1)
		GET_MANT_64(m2, r2)
	} else if (e2 < e1) {
		r1 = a;
		BIDECIMAL_CALL2 (bid64_quantize, r2, b, a);
		GET_EXP_64(e1, r1)
		GET_EXP_64(e2, r2)
		GET_MANT_64(m1, r1)
		GET_MANT_64(m2, r2)
	} else {
		r1 = a;
		r2 = b;
	}

	if (e1 != e2) {
		printf("ERROR a, b "BID_FMT_LLX16" "BID_FMT_LLX16"\n", a, b);
		printf("ERROR r1, r2 "BID_FMT_LLX16" "BID_FMT_LLX16"\n", r1, r2);
		return 1;
	}

	ulp = m1 > m2 ? m1 - m2 : m2 - m1;
   	BIDECIMAL_CALL2_NORND (bid64_quiet_less, less, a, b);
	if (less) ulp *= -1.0;
//printf("ulp %f +add %f max %f\n", ulp, ulp+ulp_add, mre_max[rnd_mode]);
	if (fabs(ulp+ulp_add) > mre_max[rnd_mode]) {
		return 1;
	}

	return 0;
}

int
check32_rel(BID_UINT32 a32, BID_UINT32 b32)
{
	BID_UINT32 r1, r2;
	BID_UINT32 e1, e2, m1, m2;
	int sign, less;
	int aexp, bexp, amant, bmant;
        int t1, t2, t3, t4;
 
        BIDECIMAL_CALL1_NORND_NOSTAT (bid32_isNaN, t1, a32);
        BIDECIMAL_CALL1_NORND_NOSTAT (bid32_isNaN, t2, b32);
        BIDECIMAL_CALL1_NORND_NOSTAT (bid32_isInf, t3, a32);
        BIDECIMAL_CALL1_NORND_NOSTAT (bid32_isInf, t4, b32);
 
   //if (bid32_isNaN(a32)||bid32_isNaN(b32)||bid32_isInf(a32)||bid32_isInf(b32)) {
   if (t1 || t2 || t3 || t4) {

       if (a32 == b32) return 0;
		else return 1;
		
	}

	if ((a32 & 0x80000000) != (b32 & 0x80000000)) {
		return 1;
	}
	if (a32 & 0x80000000) sign = 1;

	GET_EXP_32(e1, a32)
	GET_EXP_32(e2, b32)
	GET_MANT_32(m1, a32)
	GET_MANT_32(m2, b32)

	if (e1 < e2) {
		BIDECIMAL_CALL2 (bid32_quantize, r1, a32, b32);
		r2 = b32;
		GET_EXP_32(e1, r1)
		GET_EXP_32(e2, r2)
		GET_MANT_32(m1, r1)
		GET_MANT_32(m2, r2)
	} else if (e2 < e1) {
		r1 = a32;
		BIDECIMAL_CALL2 (bid32_quantize, r2, b32, a32);
		GET_EXP_32(e1, r1)
		GET_EXP_32(e2, r2)
		GET_MANT_32(m1, r1)
		GET_MANT_32(m2, r2)
	} else {
		r1 = a32;
		r2 = b32;
	}

	if (e1 != e2) {
		printf("ERROR a, b %08x %08x\n", a32, b32);
		printf("ERROR r1, r2 %08x %08x\n", r1, r2);
		return 1;
	}

	ulp = m1 > m2 ? m1 - m2 : m2 - m1;
   	BIDECIMAL_CALL2_NORND (bid32_quiet_less, less, a32, b32);
	if (less) ulp *= -1.0;
//printf("ulp %f +add %f max %f\n", ulp, ulp+ulp_add, mre_max[rnd_mode]);
	if (fabs(ulp+ulp_add) > mre_max[rnd_mode]) {
		return 1;
	}

	return 0;
}


void
get_ops (void) {
  unsigned int save_rnd = rnd;
  rnd = rnd_mode = BID_ROUNDING_TO_NEAREST;

  // Initialize second arg for comparing - required by modf function but can give false errors if argument is not initialized
  B32 = 0;
  B64 = 0ull;
  B.w[BID_LOW_128W] = B.w[BID_HIGH_128W] = 0ull;

  strcpy (istr1, "");
  strcpy (istr2, "");
  strcpy (istr3, "");

  switch (op1type) {
  case OP_DEC32:
    getop32 (A32, a32, op1, istr1);
    break;
  case OP_DEC64:
    getop64 (A64, a64, op1, istr1);
    break;
  case OP_DEC128:
    getop128 (A, a, op1, istr1);
    break;
  case OP_INT8:
    getop8i (AI32, op1, istr1);
    AI8=(char)AI32;
    break;
  case OP_INT16:
    getop16i (AI32, op1, istr1);
    AI16=(short)AI32;
    break;
  case OP_INT32:
    getop32i (AI32, op1, istr1);
    break;
  case OP_BID_UINT8:
    getop8u (AUI32, op1, istr1);
    AUI8=(unsigned char)AUI32;
    break;
  case OP_BID_UINT16:
    getop16u (AUI32, op1, istr1);
    AUI16=(unsigned short)AUI32;
    break;
  case OP_BID_UINT32:
    getop32u (AUI32, op1, istr1);
    break;
  case OP_INT64:
    getop64i (AI64, op1, istr1);
    break;
  case OP_BID_UINT64:
    getop64u (AUI64, op1, istr1);
    break;
  case OP_BIN128:
    getopquad (Aquad, Aquad, op1, istr1);
    break;
  case OP_BIN80:
    getopldbl (Aldbl, Aldbl, op1, istr1);
    break;
  case OP_BIN64:
    getopdbl (Adbl, Adbl, op1, istr1);
    break;
  case OP_BIN32:
    getopflt (Aflt, Aflt, op1, istr1);
    break;
  case OP_NONE:
    break;
  default:
    printf ("Error: getops unexpected operand 1 type, %d\n", op1type);
  }

  switch (op2type) {
  case OP_DEC32:
    getop32 (B32, b32, op2, istr2);
    break;
  case OP_DEC64:
    getop64 (B64, b64, op2, istr2);
    break;
  case OP_DEC128:
    getop128 (B, b, op2, istr2);
    break;
  case OP_INT16:
    getop16i (BI32, op2, istr2);
    BI16=(short)BI32;
    break;
  case OP_INT32:
    getop32i (BI32, op2, istr2);
    break;
  case OP_BID_UINT32:	//  Skip because converted with sscanf
    getop32u (BUI32, op2, istr2);
    break;
  case OP_LINT:	
    if (li_size_run == 64) {
	    getop64i (*(BID_SINT64*)&BLI, op2, istr2);
	} else {
	    getop32i (*(BID_SINT32*)&BLI, op2, istr2);
	}
    break;
  case OP_NONE:
    break;
  default:
    printf ("Error: getops unexpected operand 2 type, %d\n", op2type);
  }

  switch (op3type) {
  case OP_DEC32:
    getop32 (C32, c32, op3, istr3);
    break;
  case OP_DEC64:
    getop64 (CC64, c64, op3, istr3);
    break;
  case OP_DEC128:
    getop128 (C, c, op3, istr3);
    break;
  case OP_BID_UINT32:	//  Skip because converted with sscanf
    getop32u (CUI32, op3, istr3);
    break;
  case OP_NONE:
    break;
  default:
    printf ("Error: getops unexpected operand 3 type, %d\n", op3type);
    exit (-1);
  }
  rnd = rnd_mode = save_rnd;

  // true second res of frexp function
  if (!strcmp (func, "bid32_frexp") || !strcmp (func, "bid64_frexp") || !strcmp (func, "bid128_frexp")) {
	i1 = BI32;
  }
  // true second res of modf function
  R32_1 = B32;
  R64_1 = B64;
  R_1 = B;

}

void
get_test (void) {

	//initialize results so we can easy compare frexp function
	i1 = i2 = 0;
	u1_16 = u2_16 = 0;
	i1_16 = i2_16 = 0;
	u1_8 = u2_8 = 0;
	i1_8 = i2_8 = 0;
	Qi64 = qi64 = 0;
	
  get_ops ();

  switch (restype) {
  case OP_DEC32:
  case OP_DPD32:
    getop32 (R32, q32, res, rstr);
    break;
  case OP_DEC64:
  case OP_DPD64:
    getop64 (R64, q64, res, rstr);
    break;
  case OP_DEC128:
  case OP_DPD128:
    getop128 (R, q, res, rstr);
    break;
  case OP_INT8:
    getop8i (i1, res, rstr);
    break;
  case OP_INT16:
    getop16i (i1, res, rstr);
    break;
  case OP_INT32:	//  Skip because converted with sscanf
    getop32i (i1, res, rstr);
    break;
  case OP_BID_UINT8:
    getop8u (i1, res, rstr);
    break;
  case OP_BID_UINT16:
    getop16u (i1, res, rstr);
    break;
  case OP_BID_UINT32:
    getop32u (i1, res, rstr);
    break;
  case OP_INT64:
    getop64i (qi64, res, rstr);
    break;
  case OP_BID_UINT64:
    getop64u (qi64, res, rstr);
    break;
  case OP_BIN128:
    getopquad (Rtquad, Rtquad, res, rstr);
    break;
  case OP_BIN80:
    getopldbl (Rtldbl, Rtldbl, res, rstr);
    break;
  case OP_BIN64:
    getopdbl (Rtdbl, Rtdbl, res, rstr);
    break;
  case OP_BIN32:
    getopflt (Rtflt, Rtflt, res, rstr);
    break;
  case OP_LINT:	
    if (li_size_run == 64) {
	    getop64i (*(BID_SINT64*)&li1, res, rstr);
	} else {
	    getop32i (*(BID_SINT32*)&li1, res, rstr);
	}
    break;
  case OP_NONE:
    break;
  default:
    printf ("Error: get_test unexpected result type, %d\n", restype);
  }
}

void
print_mismatch (enum _CMPTYPE cmp) {
  int i;
  char str[STRMAX];

  if (answer_opt) {
    if (*op3)
      printf ("test%d %s %s %s %s ", tests, func, op1, op2, op3);
    else if (*op2)
      printf ("test%d %s %s %s ", tests, func, op1, op2);
    else
      printf ("test%d %s %s ", tests, func, op1);
    switch (restype) {

    case OP_STRING:
printf("STRING result is not implemented\n");
		break;	
    case OP_DEC128:
    case OP_DPD128:
      printf ("[" BID_FMT_LLX16 "" BID_FMT_LLX16 "] %02x\n", Q.w[BID_HIGH_128W],
	      Q.w[BID_LOW_128W], *pfpsf);
      break;

    case OP_DEC64:
    case OP_DPD64:
      printf ("[" BID_FMT_LLX16 "] %02x\n", Q64, *pfpsf);
      break;

    case OP_DEC32:
    case OP_DPD32:
      printf ("[" BID_FMT_X8 "] %02x\n", Q32, *pfpsf);
      break;

    case OP_INT8:
    case OP_INT16:
    case OP_INT32:
      printf ("%d %02x\n", i2, *pfpsf);
      break;

    case OP_BID_UINT8:
    case OP_BID_UINT16:
    case OP_BID_UINT32:
      printf ("%u %02x\n", i2, *pfpsf);
      break;

    case OP_INT64:
      printf (BID_FMT_LLD16 " %02x\n", Qi64, *pfpsf);
      break;

    case OP_LINT:
      if (li_size_run == 64)	
	      printf (BID_FMT_LLD16 " %02x\n", *(BID_SINT64*)&li2, *pfpsf);
      else
	      printf ("%d %02x\n", *(BID_SINT32*)&li2, *pfpsf);
      break;

    case OP_BID_UINT64:
      printf (BID_FMT_LLU16 " %02x\n", Qi64, *pfpsf);
      break;

    case OP_BIN128:
      printf ("[" BID_FMT_LLX16 " " BID_FMT_LLX16 "] %02x\n",
	      *((BID_UINT64 *) & Rquad + BID_HIGH_128W),
	      *((BID_UINT64 *) & Rquad + BID_LOW_128W), *pfpsf);
      break;

    case OP_BIN80:
#if BID_BIG_ENDIAN
      printf ("[" BID_FMT_LLX16 "" BID_FMT_X4 "] %02x\n", *((BID_UINT64 *) & Rldbl),
	      ((BID_UINT32) (*((BID_UINT64 *) & Rldbl + 1))) & 0xffff, *pfpsf);
#else
      printf ("[" BID_FMT_X4 "" BID_FMT_LLX16 "] %02x\n",
	      ((BID_UINT32) (*((BID_UINT64 *) & Rldbl + 1))) & 0xffff,
	      *((BID_UINT64 *) & Rldbl), *pfpsf);
#endif
      break;

    case OP_BIN64:
      printf ("[" BID_FMT_LLX16 "] %02x\n", *((BID_UINT64 *) & Rdbl), *pfpsf);
      break;

    case OP_BIN32:
      printf ("[" BID_FMT_X8 "] %02x\n", *((BID_UINT32 *) & Rflt), *pfpsf);
      break;

    default:
      printf ("print_mismatch: unknown result type %d\n", restype);
    }
    return;
  }

  // Print mismatch line
  switch (restype) {
  case OP_DEC128:
  case OP_DPD128:
    printf ("// BID result: " BID_FMT_LLX16 "" BID_FMT_LLX16
	    ", Expected results: " BID_FMT_LLX16 "" BID_FMT_LLX16 "\n",
	    Q.w[BID_HIGH_128W], Q.w[BID_LOW_128W], R.w[BID_HIGH_128W],
	    R.w[BID_LOW_128W]);
	if (!strcmp(func, "bid128_modf")) {
	    printf ("// BID second result: " BID_FMT_LLX16 "" BID_FMT_LLX16
		    ", Expected results: " BID_FMT_LLX16 "" BID_FMT_LLX16 "\n",
	    	B.w[BID_HIGH_128W], B.w[BID_LOW_128W], R_1.w[BID_HIGH_128W],
		    R_1.w[BID_LOW_128W]);
	}
    break;
  case OP_DEC64:
  case OP_DPD64:
    printf ("// BID result: " BID_FMT_LLX16 ", Expected result: " BID_FMT_LLX16
	    "\n", Q64, R64);
	if (!strcmp(func, "bid64_modf")) {
	    printf ("// BID second result: " BID_FMT_LLX16 ", Expected result: " BID_FMT_LLX16
		    "\n", B64, R64_1);
	}
    break;
  case OP_DEC32:
  case OP_DPD32:
    printf ("// BID result: " BID_FMT_X8 ", Expected result: " BID_FMT_X8 "\n",
	    Q32, R32);
	if (!strcmp(func, "bid32_modf")) {
	    printf ("// BID second result: " BID_FMT_X8 ", Expected result: " BID_FMT_X8 "\n",
		    B32, R32_1);
	}
    break;
  case OP_INT8:
  case OP_INT16:
  case OP_INT32:
    printf ("// BID result: %d, Expected result: %d\n", i2, i1);
    break;
  case OP_BID_UINT8:
  case OP_BID_UINT16:
  case OP_BID_UINT32:
    printf ("// BID result: %u, Expected result: %u\n", i2, i1);
    break;
  case OP_INT64:
    printf ("// BID result: " BID_FMT_LLD16 ", Expected result: " BID_FMT_LLD16
	    "\n", Qi64, qi64);
    break;
  case OP_LINT:
      if (li_size_run == 64)	
	    printf ("// BID result: " BID_FMT_LLD16 ", Expected result: " BID_FMT_LLD16
	    "\n",*(BID_SINT64*)&li2, *(BID_SINT64*)&li1);
      else
	    printf ("// BID result: %d, Expected result: %d \n",*(BID_SINT32*)&li2, *(BID_SINT32*)&li1);
      break;
  case OP_BID_UINT64:
    printf ("// BID result: " BID_FMT_LLU16 ", Expected result: " BID_FMT_LLU16
	    "\n", Qi64, qi64);
    break;
  case OP_BIN128:
    printf ("// BID result: " BID_FMT_LLX16 " " BID_FMT_LLX16
	    ", Expected result: " BID_FMT_LLX16 " " BID_FMT_LLX16 "\n",
	    *((BID_UINT64 *) & Rquad + BID_HIGH_128W),
	    *((BID_UINT64 *) & Rquad + BID_LOW_128W),
	    *((BID_UINT64 *) & Rtquad + BID_HIGH_128W),
	    *((BID_UINT64 *) & Rtquad + BID_LOW_128W));
    break;
  case OP_BIN80:
#if BID_BIG_ENDIAN
    printf ("// BID result: " BID_FMT_LLX16 "" BID_FMT_X4 ", Expected result: "
	    BID_FMT_LLX16 "" BID_FMT_X4 " \n", *((BID_UINT64 *) & Rldbl + 0),
	    ((BID_UINT32) (*((BID_UINT64 *) & Rldbl + 1))) & 0xffff,
	    *((BID_UINT64 *) & Rtldbl + 0),
	    ((BID_UINT32) (*((BID_UINT64 *) & Rtldbl + 1))) & 0xffff);
#else
    printf ("// BID result: " BID_FMT_X4 "" BID_FMT_LLX16 ", Expected result: "
	    BID_FMT_X4 "" BID_FMT_LLX16 " \n",
	    ((BID_UINT32) (*((BID_UINT64 *) & Rldbl + 1))) & 0xffff,
	    *((BID_UINT64 *) & Rldbl + 0),
	    ((BID_UINT32) (*((BID_UINT64 *) & Rtldbl + 1))) & 0xffff,
	    *((BID_UINT64 *) & Rtldbl + 0));
#endif
    break;
  case OP_BIN64:
    printf ("// BID result: " BID_FMT_LLX16 ", Expected result: " BID_FMT_LLX16
	    "\n", *((BID_UINT64 *) & Rdbl), *((BID_UINT64 *) & Rtdbl));
    break;
  case OP_BIN32:
    printf ("// BID result: " BID_FMT_X8 ", Expected result: " BID_FMT_X8 "\n",
	    *((BID_UINT32 *) & Rflt), *((BID_UINT32 *) & Rtflt));
    break;
  default:
    printf ("print_mismatch: unknown result type %d\n", restype);
    exit (-1);
  }

  if (!strcmp(func, "bid32_frexp") || !strcmp(func, "bid64_frexp") || !strcmp(func, "bid128_frexp") ) {
    printf ("// BID second result: %d, Expected result: %d\n", i2, i1);
  }
	
  // Print statuses
  printf ("// BID status : %03x, Expected status: %03x (PUOZDI Bits)\n",
	  *pfpsf, expected_status);

  switch (restype) {
  case OP_DEC128:
    BIDECIMAL_CALL1_NORND_RESREF (bid128_to_string, str1, Q);
    BIDECIMAL_CALL1_NORND_RESREF (bid128_to_string, str2, R);
    printf ("// Result BID128 String: %s\n", str1);
    printf ("// Expected BID128 String: %s\n", str2);
    break;
  case OP_DEC64:
    BIDECIMAL_CALL1_NORND_RESREF (bid64_to_string, str1, Q64);
    BIDECIMAL_CALL1_NORND_RESREF (bid64_to_string, str2, R64);
    printf ("// Result BID64 String: %s\n", str1);
    printf ("// Expected BID64 String: %s\n", str2);
    break;
  case OP_DEC32:
    BIDECIMAL_CALL1_NORND_RESREF (bid32_to_string, str1, Q32);
    BIDECIMAL_CALL1_NORND_RESREF (bid32_to_string, str2, R32);
    printf ("// Result BID32 String: %s\n", str1);
    printf ("// Expected BID32 String: %s\n", str2);
    break;
  case OP_DPD128:
    {
      BID_UINT128 Qbid, Rbid;
      BIDECIMAL_CALL1_NORND_NOSTAT (bid_dpd_to_bid128, Qbid, Q);
      BIDECIMAL_CALL1_NORND_NOSTAT (bid_dpd_to_bid128, Rbid, R);
      BIDECIMAL_CALL1_NORND_RESREF (bid128_to_string, str2, Qbid);
      BIDECIMAL_CALL1_NORND_RESREF (bid128_to_string, str2, Rbid);
      printf ("// Result BID128 String: %s\n", str1);
      printf ("// Expected BID128 String: %s\n", str2);
    }
    break;
  case OP_DPD64:
    {
      BID_UINT64 Qbid, Rbid;
      BIDECIMAL_CALL1_NORND_NOSTAT (bid_dpd_to_bid64, Qbid, Q64);
      BIDECIMAL_CALL1_NORND_NOSTAT (bid_dpd_to_bid64, Rbid, R64);
      BIDECIMAL_CALL1_NORND_RESREF (bid64_to_string, str1, Qbid);
      BIDECIMAL_CALL1_NORND_RESREF (bid64_to_string, str2, Rbid);
      printf ("// Result BID64 String: %s\n", str1);
      printf ("// Expected BID64 String: %s\n", str2);
    }
    break;
  case OP_DPD32:
    {
      BID_UINT32 Qbid, Rbid;
      BIDECIMAL_CALL1_NORND_NOSTAT (bid_dpd_to_bid32, Qbid, Q32);
      BIDECIMAL_CALL1_NORND_NOSTAT (bid_dpd_to_bid32, Rbid, R32);
      BIDECIMAL_CALL1_NORND_RESREF (bid32_to_string, str1, Qbid);
      BIDECIMAL_CALL1_NORND_RESREF (bid32_to_string, str2, Rbid);
      printf ("// Result BID32 String: %s\n", str1);
      printf ("// Expected BID32 String: %s\n", str2);
    }
    break;
  case OP_INT8:
  case OP_BID_UINT8:
  case OP_INT16:
  case OP_BID_UINT16:
  case OP_INT32:
  case OP_BID_UINT32:
  case OP_INT64:
  case OP_BID_UINT64:
  case OP_LINT:
    break;
  case OP_BIN128:
//        sprintf(str1, "%.17le", Rquad); \
//        sprintf(str2, "%.17le", Rtquad); \

    strcpy (str1, "unavailable");
    strcpy (str2, "unavailable");
    printf ("// BID128 quad res: %s\n", str1);
    printf ("// Expected quad res : %s\n", str2);
    break;
  case OP_BIN80:
    sprintf (str1, "%.17le", (double) Rldbl);
    sprintf (str2, "%.17le", (double) Rtldbl);
    printf ("// BID80 double res (cast to double): %s\n", str1);
    printf ("// Expected double res (cast to double): %s\n", str2);
    break;
  case OP_BIN64:
    sprintf (str1, "%.17le", Rdbl);
    sprintf (str2, "%.17le", Rtdbl);
    printf ("// BID64 double res: %s\n", str1);
    printf ("// Expected double res : %s\n", str2);
    break;
  case OP_BIN32:
    sprintf (str1, "%.9e", Rflt);
    sprintf (str2, "%.9e", Rtflt);
    printf ("// BID32 double res: %s\n", str1);
    printf ("// decimal32 double res : %s\n", str2);
    break;
  default:
    printf ("print_mismatch unknown result type %d\n", restype);
    exit (-1);
  }

  printf ("// Input operand strings: %s %s %s\n", istr1, istr2, istr3);
  fail_res++;
  sprintf (line, "%s %s %s %s\n", func, op1, op2, op3);
    printf ("// Ulp error: %e\n", ulp+ulp_add);
	printf ("// Full input string: %s\n", full_line);
    printf ("// Input string number: %d\n", line_counter);
  printf ("// FAILED(%s)\n\n", rounding);
}

void
check_results (enum _CMPTYPE cmp) {
  char *p;
  tests++;
	
//printf("frexp i1 i2 %d %d \n", i1, i2);

//printf("arg dbl %08X \n", *((int*)&Adbl+BID_HIGH_128W));
  if (p = strstr (func, "binary64_to")) {
//printf("check fo binary snan\n");
		if (SNaN_passed_incorrectly64 && ((*((int*)&Adbl+BID_HIGH_128W) & 0x7ff80000) == 0x7ff00000) &&
		((*((int*)&Adbl+BID_HIGH_128W) & 0x0007ffff) || (*((int*)&Adbl+BID_LOW_128W) & 0xffffffff))
		) {
//printf("set invalid for 64\n");
			*pfpsf |= BID_INVALID_EXCEPTION;
		}
  }
  if (p = strstr (func, "binary32_to")) {
		if (SNaN_passed_incorrectly32 && ((*((int*)&Aflt) & 0x7fc00000) == 0x7f800000) &&
		((*((int*)&Aflt) & 0x003fffff))
		) {
//printf("set invalid for 32\n");
			*pfpsf |= BID_INVALID_EXCEPTION;
		}
  }
  if (p = strstr (func, "binary80_to")) {
//printf("check fo binary snan\n");
		if (SNaN_passed_incorrectly80 && ((*((int*)&Aldbl+2) & 0x7fff) == 0x7fff) &&
		((*((int*)&Aldbl+BID_HIGH_128W) & 0xc0000000) == 0x80000000) &&
		((*((int*)&Aldbl+BID_HIGH_128W) & 0x7fffffff) || (*((int*)&Aldbl+BID_LOW_128W) & 0xffffffff))
		) {
//printf("set invalid for 80\n");
			*pfpsf |= BID_INVALID_EXCEPTION;
		}
  }

  if (check_restore_binary_status()) {
		print_mismatch(cmp);
		return;
  }

  if (debug_opt || answer_opt) {
    print_mismatch (cmp);
    return;
  }

  if (!strcmp (func, "bid64_to_string")) {
    BIDECIMAL_CALL1_RESARG (bid64_from_string, Q64, convstr);
  } else if (!strcmp (func, "bid128_to_string")) {
    BIDECIMAL_CALL1_RESARG (bid128_from_string, Q, convstr);
  } else if (!strcmp (func, "bid32_to_string")) {
    BIDECIMAL_CALL1_RESARG (bid32_from_string, Q32, convstr);
  }

  if (restype == OP_DEC128 && cmp == CMP_EXACTSTATUS) {
    if (expected_status != *pfpsf || R.w[BID_LOW_128W] != Q.w[BID_LOW_128W]
	|| R.w[BID_HIGH_128W] != Q.w[BID_HIGH_128W] || i1 != i2 || Qi64 != qi64)
      print_mismatch (cmp);
  } else if (restype == OP_DEC64 && cmp == CMP_EXACTSTATUS) {
    if (expected_status != *pfpsf || R64 != Q64 || i1 != i2 || Qi64 != qi64)
      print_mismatch (cmp);
  } else if (restype == OP_DEC32
	     && (cmp == CMP_EXACTSTATUS || cmp == CMP_FUZZYSTATUS)) {
    if (expected_status != *pfpsf || R32 != Q32 || i1 != i2 || Qi64 != qi64 || R32_1 != B32)
      print_mismatch (cmp);
  } else if (restype == OP_LINT
	     && (cmp == CMP_EXACTSTATUS || cmp == CMP_FUZZYSTATUS)) {
    if (expected_status != *pfpsf || li1 != li2)
      print_mismatch (cmp);
  } else if (restype == OP_DEC32 && (cmp == CMP_RELATIVEERR)) {
    if ((expected_status&trans_flags_mask) != (*pfpsf&trans_flags_mask) || check32_rel(R32, Q32))
      print_mismatch (cmp);
  } else if (restype == OP_DEC128 && cmp == CMP_FUZZYSTATUS) {
    if (expected_status != *pfpsf || check128 (R, Q) || i1 != i2 || Qi64 != qi64 || check128 (R_1, B))
      print_mismatch (cmp);
  } else if (restype == OP_LINT && cmp == CMP_FUZZYSTATUS) {
    if (expected_status != *pfpsf || li1 != li2)
      print_mismatch (cmp);
  } else if (restype == OP_DEC128 && cmp == CMP_RELATIVEERR) {
    if ((expected_status&trans_flags_mask) != (*pfpsf&trans_flags_mask) || check128_rel(R, Q))
      print_mismatch (cmp);
  } else if (restype == OP_DEC64 && cmp == CMP_FUZZYSTATUS) {
    if (expected_status != *pfpsf || check64 (R64, Q64) || i1 != i2 || Qi64 != qi64 || R64_1 != B64)
      print_mismatch (cmp);
  } else if (restype == OP_LINT && cmp == CMP_FUZZYSTATUS) {
    if (expected_status != *pfpsf || li1 != li2)
      print_mismatch (cmp);
  } else if (restype == OP_DEC64 && cmp == CMP_RELATIVEERR) {
    if ((expected_status&trans_flags_mask) != (*pfpsf&trans_flags_mask) || check64_rel(R64, Q64))
      print_mismatch (cmp);
  } else if (restype == OP_DEC128 && cmp == CMP_EQUALSTATUS) {
    int c;
    unsigned int tmp_pfpsf = *pfpsf;
    BIDECIMAL_CALL2_NORND (bid128_quiet_not_equal, c, Q, R);
    if (expected_status != tmp_pfpsf || (check128 (R, Q) && c))
      print_mismatch (cmp);
  } else if (restype == OP_DEC64 && cmp == CMP_EQUALSTATUS) {
    int c;
    unsigned int tmp_pfpsf = *pfpsf;
    BIDECIMAL_CALL2_NORND (bid64_quiet_not_equal, c, Q64, R64);
    if (expected_status != tmp_pfpsf || (check64 (R64, Q64) && c))
      print_mismatch (cmp);
  } else if (restype == OP_DEC32 && cmp == CMP_EQUALSTATUS) {
    int c;
    unsigned int tmp_pfpsf = *pfpsf;
    BIDECIMAL_CALL2_NORND (bid32_quiet_not_equal, c, Q32, R32);
    if (expected_status != tmp_pfpsf || (check32 (R32, Q32) && c))
      print_mismatch (cmp);
  } else if (restype == OP_DPD32 && cmp == CMP_FUZZYSTATUS) {
    if (expected_status != *pfpsf || check32 (R32, Q32))
      print_mismatch (cmp);
  } else if (restype == OP_DPD64 && cmp == CMP_FUZZYSTATUS) {
    if (expected_status != *pfpsf || check64 (R64, Q64))
      print_mismatch (cmp);
  } else if (restype == OP_DPD128 && cmp == CMP_FUZZYSTATUS) {
    if (expected_status != *pfpsf || check128 (R, Q))
      print_mismatch (cmp);
  } else if (restype == OP_INT32 || restype == OP_INT16
	     || restype == OP_INT8) {
    if (expected_status != *pfpsf || i1 != i2)
      print_mismatch (cmp);
  } else if (restype == OP_BID_UINT32 || restype == OP_BID_UINT16
	     || restype == OP_BID_UINT8) {
    if (expected_status != *pfpsf || i1 != i2)
      print_mismatch (cmp);
  } else if (restype == OP_INT64) {
    if (expected_status != *pfpsf || qi64 != Qi64)
      print_mismatch (cmp);
  } else if (restype == OP_BID_UINT64) {
    if (expected_status != *pfpsf || qi64 != Qi64)
      print_mismatch (cmp);
  } else if (restype == OP_BIN128) {
    if (expected_status != *pfpsf
	|| *((BID_UINT64*)&Rquad) != *((BID_UINT64*)&Rtquad)
	|| *((BID_UINT64*)&Rquad+1) != *((BID_UINT64*)&Rtquad+1))
      print_mismatch (cmp);
  } else if (restype == OP_BIN80) {
    if (expected_status != *pfpsf
	|| *((BID_UINT64 *) & Rldbl) != *((BID_UINT64 *) & Rtldbl)
	|| (*((BID_UINT64 *) & Rldbl + 1) & 0xffff) !=
	(*((BID_UINT64 *) & Rtldbl + 1) & 0xffff))
      print_mismatch (cmp);
  } else if (restype == OP_BIN64) {
    if (expected_status != *pfpsf
	|| (*(BID_UINT64 *) & Rdbl != *(BID_UINT64 *) & Rtdbl))
      print_mismatch (cmp);
  } else if (restype == OP_BIN32) {
    if (expected_status != *pfpsf
	|| (*(BID_UINT32 *) & Rflt != *(BID_UINT32 *) & Rtflt))
      print_mismatch (cmp);
  } else if (restype == OP_DPD64 && cmp == CMP_EXACTSTATUS) {
    if (expected_status != *pfpsf || R64 != Q64)
      print_mismatch (cmp);
  } else if (restype == OP_DPD32 && cmp == CMP_EXACTSTATUS) {
    if (expected_status != *pfpsf || R32 != Q32)
      print_mismatch (cmp);
  } else if (restype == OP_DPD128 && cmp == CMP_EXACTSTATUS) {
    if (expected_status != *pfpsf || R.w[BID_LOW_128W] != Q.w[BID_LOW_128W]
	|| R.w[BID_HIGH_128W] != Q.w[BID_HIGH_128W])
      print_mismatch (cmp);
  } else {
    printf
      ("Unknown combination of result type (%d) and compare type (%d)\n",
       restype, cmp);
    // exit (-1);
  }
}

// int st_compare(void *a, void *b) {
int
st_compare (char **a, char **b) {
  return strcmp (*a, *b);
}

int
status_compare (char *stat1, char *stat2) {
  char s1[STRMAX], s2[STRMAX];
  char *wp1[64], *wp2[64];
  int wp1n = 0, wp2n = 0;
  char *p;
  int i;

  strcpy (s1, stat1);
  strcpy (s2, stat2);
  for (p = s1; *p; p++) {
    wp1[wp1n++] = p;
    while (*p && *++p != ' ');
    if (*p == ' ') {
      *p = 0;
      // printf("Found 1: %s\n", wp1[wp1n-1]);
      continue;
    } else {
      // printf("Found 1: %s\n", wp1[wp1n-1]);
      break;
    }
  }
  for (p = s2; *p; p++) {
    wp2[wp2n++] = p;
    while (*p && *++p != ' ');
    if (*p == ' ') {
      *p = 0;
      // printf("Found 2: %s\n", wp2[wp2n-1]);
      continue;
    } else {
      // printf("Found 2: %s\n", wp2[wp2n-1]);
      break;
    }
  }
  if (wp1n != wp2n)
    return 1;
#ifdef LINUX
  qsort (wp1, wp1n, sizeof (char *), (__compar_fn_t) st_compare);
  qsort (wp2, wp2n, sizeof (char *), (__compar_fn_t) st_compare);
#else
  qsort (wp1, wp1n, sizeof (char *),
	 (int (__cdecl *) (const void *, const void *)) st_compare);
  qsort (wp2, wp2n, sizeof (char *),
	 (int (__cdecl *) (const void *, const void *)) st_compare);
#endif
  for (i = 0; i < wp1n; i++) {
    // printf("Comparing %s and %s\n", wp1[i], wp2[i]);
    if (strcmp (wp1[i], wp2[i]))
      return 1;
  }
  return 0;
}

int check_skip(char *func)
{
	if (no128trans) {
		if (!strcmp(func, "bid128_sin") || !strcmp(func, "bid128_cos") || !strcmp(func, "bid128_tan") ||
			!strcmp(func, "bid128_asin") || !strcmp(func, "bid128_acos") || !strcmp(func, "bid128_atan") ||
			!strcmp(func, "bid128_sinh") || !strcmp(func, "bid128_cosh") || !strcmp(func, "bid128_tanh") ||
			!strcmp(func, "bid128_asinh") || !strcmp(func, "bid128_acosh") || !strcmp(func, "bid128_atanh") ||
			!strcmp(func, "bid128_exp") || !strcmp(func, "bid128_expm1") ||
			!strcmp(func, "bid128_log") || !strcmp(func, "bid128_log10") || !strcmp(func, "bid128_log1p") ||
			!strcmp(func, "bid128_atan2") || !strcmp(func, "bid128_hypot") || !strcmp(func, "bid128_pow") ||
			!strcmp(func, "bid128_cbrt") 
		) return 1;
	}
	if (no64trans) {
		if (!strcmp(func, "bid64_sin") || !strcmp(func, "bid64_cos") || !strcmp(func, "bid64_tan") ||
			!strcmp(func, "bid64_asin") || !strcmp(func, "bid64_acos") || !strcmp(func, "bid64_atan") ||
			!strcmp(func, "bid64_sinh") || !strcmp(func, "bid64_cosh") || !strcmp(func, "bid64_tanh") ||
			!strcmp(func, "bid64_asinh") || !strcmp(func, "bid64_acosh") || !strcmp(func, "bid64_atanh") ||
			!strcmp(func, "bid64_exp") || !strcmp(func, "bid64_expm1") ||
			!strcmp(func, "bid64_log") || !strcmp(func, "bid64_log10") || !strcmp(func, "bid64_log1p") ||
			!strcmp(func, "bid64_atan2") || !strcmp(func, "bid64_hypot") || !strcmp(func, "bid64_pow") ||
			!strcmp(func, "bid64_cbrt") 
		) return 1;
	}
	if (li_size_run != li_size_test) return 1;
	return 0;
}


void check_snan_passing32(float x)
{
	int *px = (int*)&x;
	if (*(px) & 0x00400000) SNaN_passed_incorrectly32 = 1;
}

void check_snan_passing64(double x)
{
	int *px = (int*)&x;
	if (*(px+BID_HIGH_128W) & 0x00080000) SNaN_passed_incorrectly64 = 1;
}

void check_snan_passing80(long double x)
{
	int *px = (int*)&x;
#if BID_BIG_ENDIAN
	if (*(px+BID_HIGH_128W) & 0x40000000) SNaN_passed_incorrectly80 = 1;
#else
	if (*(px) & 0x00008000) SNaN_passed_incorrectly80 = 1;
#endif

}

void check_den_passing32(float x)
{
#if !defined _MSC_VER && !defined __INTEL_COMPILER
	fexcept_t ff; 
	fegetexceptflag(&ff, FE_ALL_EXCEPT);
	if (ff & FE_DENORMAL) Den_passed_incorrectly32 = 1;
#endif
}
void check_den_passing64(double x)
{
#if !defined _MSC_VER && !defined __INTEL_COMPILER
	fexcept_t ff; 
	fegetexceptflag(&ff, FE_ALL_EXCEPT);
	if (ff & FE_DENORMAL) Den_passed_incorrectly64 = 1;
#endif
}
void check_den_passing80(long double x)
{
#if !defined _MSC_VER && !defined __INTEL_COMPILER
	fexcept_t ff; 
	fegetexceptflag(&ff, FE_ALL_EXCEPT);
	if (ff & FE_DENORMAL) Den_passed_incorrectly80 = 1;
#endif
}


int
main (int argc, char *argv[]) {
  int ch, digit_optind = 0;
  int skip_test;
  char **arg;
  char *end_of_args = (char*)-1;

  strcpy (rounding, "half_even");

  if (sizeof(long int) == 8) {
	li_size_test = 64; 
	li_size_run = 64; 
  } else {
	li_size_test = 32; 
	li_size_run = 32; 
  }	

  arg = argv + 1;	// point to first command-line parameter
  while (*arg && **arg == '-') {	// Process all options
    if (strcmp (*arg, "-d") == 0)
      debug_opt = 1;
    if (strcmp (*arg, "-ub") == 0)
      underflow_before_opt = 1;
    if (strcmp (*arg, "-ua") == 0)
      underflow_after_opt = 1;
    if (strcmp (*arg, "-h") == 0) {
      printf ("Usage: runtests [-d]\n");
      exit (0);
    }
    if (strcmp (*arg, "-ulp") == 0) {
        arg++;
        argc--;
        sscanf(*arg, "%lf", &mre_max[0]);
        mre_max[4] = mre_max[3] = mre_max[2] = mre_max[1] = mre_max[0];
	}
   if (strcmp (*arg, "-bin_flags") == 0) {
        check_binary_flags_opt = 1;
        arg++;
        argc--;
        sscanf(*arg, "%x", (int*)&ini_binary_flags);
	}
    if (strcmp (*arg, "-no128trans") == 0) {
		no128trans = 1;
	}
    if (strcmp (*arg, "-no64trans") == 0) {
		no64trans = 1;
	}
    arg++;
  }

  if (underflow_before_opt && underflow_after_opt) {
    printf("Both underflow before and after rounding checking mode set, please specify just one.\n");	
    printf ("Usage: runtests [-d]\n");
    exit (0);
  } else if (!underflow_before_opt && !underflow_after_opt) underflow_before_opt = 1;

  rnd_mode = 0;
  rnd_mode |= BID_ROUNDING_TO_NEAREST;

  *((int*)&snan_check64+BID_HIGH_128W) = 0x7ff00000;
  *((int*)&snan_check64+BID_LOW_128W) =  0x00000001;
  check_snan_passing64(snan_check64);
  *((int*)&snan_check32) = 0x7f800010;
  check_snan_passing32(snan_check32);
#if BID_BIG_ENDIAN
  *((int*)&snan_check80) = 0x7fff8000;
  *((int*)&snan_check80+1) = 0;
  *((short*)&snan_check80+4) = 1;
#else
  *((short*)&snan_check80+4) = 0x7fff;
  *((int*)&snan_check80+1) = 0x80000000;
  *((int*)&snan_check80+0) =  0x00000001;
#endif
  check_snan_passing80(snan_check80);

  *((int*)&snan_check64+BID_HIGH_128W) = 0x00000000;
  *((int*)&snan_check64+BID_LOW_128W) =  0x00000001;
#if !defined _MSC_VER && !defined __INTEL_COMPILER
  feclearexcept (FE_ALL_EXCEPT);
  check_den_passing64(snan_check64);
  *((int*)&snan_check32) = 0x00000001;
  feclearexcept (FE_ALL_EXCEPT);
  check_den_passing32(snan_check32);
#endif
#if BID_BIG_ENDIAN
  *((short*)&snan_check80+4) = 0x0000;
  *((int*)&snan_check80+1) = 0x00000000;
  *((int*)&snan_check80+0) =  0x00000001;
#else
  *((int*)&snan_check80) = 0x00000000;
  *((int*)&snan_check80+1) = 0;
  *((short*)&snan_check80+4) = 1;
#endif
#if !defined _MSC_VER && !defined __INTEL_COMPILER
 feclearexcept (FE_ALL_EXCEPT);
  check_den_passing80(snan_check80);
  feclearexcept (FE_ALL_EXCEPT);
#endif
//printf("snan32 passed incorr %d\n", SNaN_passed_incorrectly32);
//printf("snan64 passed incorr %d\n", SNaN_passed_incorrectly64);
//printf("snan80 passed incorr %d\n", SNaN_passed_incorrectly80);
//printf("den32 passed incorr %d\n", Den_passed_incorrectly32);
//printf("den64 passed incorr %d\n", Den_passed_incorrectly64);
//printf("den80 passed incorr %d\n", Den_passed_incorrectly80);

  line_counter=0;
  while (!feof (stdin)) {
    int st;

    op1type = OP_NONE;
    op2type = OP_NONE;
    op3type = OP_NONE;
    restype = OP_NONE;

    fgets (line, 1023, stdin);
    line_counter++;
    strRemove0D0A(line);
	strcpy(full_line, line);
    if (feof (stdin))
      break;

    // printf("Read line: %s", line);
    if (p = strstr (line, "--")) *p = 0; // Remove comment
    strRemoveTrailingSpaces(line);

    // Reset BID status flags
    *pfpsf = 0;

    if (line[0] == 0)
      continue;

    // Extract ulp field from line, if present:
    if (p = strstr (line, "ulp=")) {
        if (p < end_of_args) end_of_args = p;
        if ( sscanf(p+4, "%le", &ulp_add) != 1 ) ulp_add=0.0;
    } else {
        ulp_add=0.0;
    }

    // Determine if underflow, indicated as expected have to be set only for before rounding mode
    if (p = strstr (line, "underflow_before_only")) {
        if (p < end_of_args) end_of_args = p;
        Underflow_Before = 1;
    } else {
        Underflow_Before = 0;
    }

	// Read string prefix for from string conversions
    if (p = strstr (line, "str_prefix=")) {
        if (p < end_of_args) end_of_args = p;
        { int i = 0; while ( *(p+12+i) != '|' && *(p+12+i) != 0 ) {
				str_prefix[i] = *(p+12+i);
				i++;
				}
				str_prefix[i] = 0;
		  }
    } else {
        strcpy(str_prefix, "");
    }

    // Extract long int size from line, if present:
    if (p = strstr (line, "longintsize=")) {
        if (p < end_of_args) end_of_args = p;
        if ( sscanf(p+12, "%d", &li_size_test) != 1 ) li_size_test=0;
    } else {
        li_size_test=li_size_run;
    }

//printf();
    if (end_of_args != (char*)-1) {	
	    *end_of_args = 0;	
   	 strRemoveTrailingSpaces(line);
   	 end_of_args = (char*)-1;
    }

    args_set = 0;
    if (sscanf (line, "%s %d %s %s %s %s %x", funcstr, &rnd_mode,
	     op1, op2, op3, res, &expected_status) == 7) {
//printf("read8 %d\n", rnd_mode);
        args_set = 1;
    }
    if (!args_set) {
        if (sscanf (line, "%s %d %s %s %s %x", funcstr, &rnd_mode,
	         op1, op2, res, &expected_status) == 6) {
//printf("read7 %d\n", rnd_mode);
            args_set = 1;
        }
	 }
    if (!args_set) {
        if (sscanf(line, "%s %d %s %s %x", funcstr, &rnd_mode, op1,
	         res, &expected_status) == 5) {
//printf("read6 %d\n", rnd_mode);
            args_set = 1;
        }
	 }

     skip_test = check_skip(funcstr);
     pollution_workaround = check_pollution_workaround();


//printf("str %s op1 %s, skip %d\n", line, op1, skip_test);
	 if (args_set && !skip_test) {
        rnd = rnd_mode;
	    // set ulp thresholds for transcendentals
    	if (p = strstr (funcstr, "128")) {
			mre_max[0] = 2.0;
			mre_max[1] = 5.0;
			mre_max[2] = 5.0;
			mre_max[3] = 5.0;
			mre_max[4] = 2.0;
    	} else if (p = strstr (funcstr, "64")) {
			mre_max[0] = 0.55;
			mre_max[1] = 1.05;
			mre_max[2] = 1.05;
			mre_max[3] = 1.05;
			mre_max[4] = 0.55;
    	} else if (p = strstr (funcstr, "32")) {
			mre_max[0] = 0.5;
			mre_max[1] = 1.01;
			mre_max[2] = 1.01;
			mre_max[3] = 1.01;
			mre_max[4] = 0.5;
		}

      strcpy (rounding, roundstr_bid[rnd]);

      //clean expected underflow if it is for before rounding mode and we are checking after rounidng 
		if ((expected_status & 0x10) && Underflow_Before && underflow_after_opt) expected_status &= ~0x00000010;

#include "readtest.h"

    } else {
      if (!skip_test) printf ("SKIPPED (line %d): %s\n", line_counter,line);
    }
  }

  printf ("Total tests: %d, failed result: %d, failed status: %d\n",
	  tests, fail_res, fail_status);
  return 0;

}

int copy_str_to_wstr() {
int i = 0;
	if (strstr (funcstr, "wcstod")) {
		while (istr1[i] != 0 && istr1[i] != '\n') {
			wistr1[i] = (wchar_t)istr1[i];
			i++;
		}
		wistr1[i] = 0;
	}
	return 1;
}


void save_binary_status()
{
#if !defined _MSC_VER && !defined __INTEL_COMPILER
	if (check_binary_flags_opt) {
	   feclearexcept (FE_ALL_EXCEPT);
		fesetexceptflag(&ini_binary_flags, FE_ALL_EXCEPT);
		fegetexceptflag(&saved_binary_flags, FE_ALL_EXCEPT);
	}
#endif
}

int check_restore_binary_status()
{
	char *p;
//printf("snan arg, passed incor %d %d\n", arg64_snan, SNaN_passed_incorrectly64);
	if (check_binary_flags_opt || debug_opt) {
#if !defined _MSC_VER && !defined __INTEL_COMPILER
		fegetexceptflag(&test_binary_flags, FE_ALL_EXCEPT);

		if (p = strstr (func, "binary32_to")) {
			if (arg32_den && Den_passed_incorrectly32) saved_binary_flags |= (test_binary_flags & FE_UNNORMAL);
			if (arg32_snan && SNaN_passed_incorrectly32) saved_binary_flags |= (test_binary_flags & FE_INVALID);
		}
		if (p = strstr (func, "binary64_to")) {
			if (arg64_den && Den_passed_incorrectly64) saved_binary_flags |= (test_binary_flags & FE_UNNORMAL);
			if (arg64_snan && SNaN_passed_incorrectly64) saved_binary_flags |= (test_binary_flags & FE_INVALID);
		}
		if (p = strstr (func, "binary80_to")) {
			if (arg80_den && Den_passed_incorrectly80) saved_binary_flags |= (test_binary_flags & FE_UNNORMAL);
			if (arg80_snan && SNaN_passed_incorrectly80) saved_binary_flags |= (test_binary_flags & FE_INVALID);
		}
// !!!! Workaround, do not favor non-standard denormal flag for now
		saved_binary_flags |= (test_binary_flags & FE_UNNORMAL);
// !!!! Workaround, do not favor non-standard denormal flag for now
		if (test_binary_flags != saved_binary_flags && !pollution_workaround ) {
			printf("// ERROR: BINARY Exception flags polluted!\n");
			printf("//        Saved value %X, value after BID call %X\n", *(int*)&saved_binary_flags, *(int*)&test_binary_flags );
			return 1;
		}
#endif
	}
	return 0;
}

int check_pollution_workaround(void)
{
	char *p;
	if ((p = strstr (func, "sin")) ||
       (p = strstr (func, "cos")) ||
       (p = strstr (func, "tan")) ||
       (p = strstr (func, "exp")) ||
       (p = strstr (func, "log")) ||
       (p = strstr (func, "erf")) ||
       (p = strstr (func, "hypot")) ||
       (p = strstr (func, "pow")) ||
       (p = strstr (func, "cbrt")) ||
       (p = strstr (func, "gamma")) 
	) {
		return 1;
	} else {
		return 0;
	}
	
}

