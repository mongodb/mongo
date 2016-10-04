/******************************************************************************
  Copyright (c) 2007-2011, Intel Corp.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without 
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, 
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright 
      notice, this list of conditions and the following disclaimer in the 
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors 
      may be used to endorse or promote products derived from this software 
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include <ctype.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "bid_internal.h"


//#define RESTRICT restrict
#define RESTRICT 

#define set_str_end(e,p)   if(e) { *e=(char*)p;  }

#define isdigit_macro(x)  ((x)>='0' && (x)<='9')

#define set_wcs_end(e,p)   if(e) { *(e)=(wchar_t*)p;  }

#define towlower_macro(x)  (((x)>=L'A' && (x)<=L'Z')? ((x)-L'A'+L'a') : (x))
#define iswdigit_macro(x)  ((x)>=L'0' && (x)<=L'9')



__BID_INLINE__ char *
strtod_conversion (const char* RESTRICT ps_in, char** RESTRICT endptr)
{
char * ps0, *ps, *ptail;


   if(!ps_in)
   {
		   if(endptr) *endptr=NULL;
		   return NULL;
   }
   while(isspace(*ps_in)) ps_in++;
   ps = malloc((strlen(ps_in)+2)*sizeof(char));
   if(!ps)
   {
		   set_str_end(endptr, ps_in);
		   return NULL;
   }
   strcpy(ps, ps_in);
   ptail = (char*)ps_in;
   ps0 = (char*)ps;
   if((*ps == '+') || (*ps=='-')) { ps++; ptail++; }

       // Infinity?
   if ((tolower_macro (ps[0]) == 'i' && tolower_macro (ps[1]) == 'n' && 
	   tolower_macro (ps[2]) == 'f')) {
		   if(tolower_macro (ps[3]) == 'i' && 
              tolower_macro (ps[4]) == 'n' && tolower_macro (ps[5]) == 'i' && 
              tolower_macro (ps[6]) == 't' && tolower_macro (ps[7]) == 'y')
		   { ps+=8;  ptail+=8;  set_str_end(endptr, ptail); }
		   else { ps+=3; ptail+=3; set_str_end(endptr, ptail);}
	   }
   else if(tolower_macro (ps[0]) == 'n' && tolower_macro (ps[1]) == 'a' && 
	   tolower_macro (ps[2]) == 'n') {
		   ps+=3; ptail+=3;
		   while(isdigit_macro(*ps)) { ps++; ptail ++; }
		   set_str_end(endptr, ptail);
		   if(*ps0=='-') strcpy(ps0,"-QNAN");
		   else strcpy(ps0, "QNAN");
	   }
   else {
	   if(!isdigit_macro(*ps) && ((*ps)!='.')) {
		   if(endptr) *endptr=(char*)ps_in;
		   free(ps0);
		   return NULL;  // no conversion
	   }
	   while(isdigit_macro(*ps)) { ps++; ptail++; }
	   if((*ps) == '.') { 
		   if((ps0!=ps) || isdigit_macro(ps[1])) {
			   ps++; ptail++; while(isdigit_macro(*ps)) {ps++; ptail++;} }
		   else {
			   if(endptr) *endptr=(char*)ps_in;
			   free(ps0);
			   return NULL;  // no conversion
		   }
	   }
	   if(tolower_macro(*ps) == 'e') { 
		   if((ps[1]=='+') || (ps[1]=='-') || (isdigit_macro(ps[1]))) { ps+=2; ptail+=2;
		   while(isdigit_macro(*ps)) {ps++; ptail++;} }
	   }
		   set_str_end(endptr, ptail);
   }

   *ps = '\0';
   return ps0;

}


__BID_INLINE__ char *
wcstod_conversion (const wchar_t* RESTRICT ps_in, wchar_t** RESTRICT endptr)
{
wchar_t * ps0, *ps, *ptail;
char* ps0_c;
int i,k;

   if(!ps_in)
   {
		   if(endptr) *endptr=NULL;
		   return NULL;
   }
   while(iswspace(*ps_in)) ps_in++;
   k = 1+wcslen(ps_in);
   ps = malloc((k+1)*sizeof(wchar_t));
   if(!ps)
   {
		   set_wcs_end(endptr,ps_in);
		   return NULL;
   }
   wcscpy(ps, ps_in);
   ptail = (wchar_t*)ps_in;
   ps0 = ps;
   if((*ps == L'+') || (*ps==L'-')) {ps++; ptail++;}

       // Infinity?
   if ((towlower_macro (ps[0]) == L'i' && towlower_macro (ps[1]) == L'n' && 
	   towlower_macro (ps[2]) == L'f')) {
		   if(towlower_macro (ps[3]) == L'i' && 
              towlower_macro (ps[4]) == L'n' && towlower_macro (ps[5]) == L'i' && 
              towlower_macro (ps[6]) == L't' && towlower_macro (ps[7]) == L'y')
		   { ps+=8; ptail+=8; set_wcs_end(endptr, ptail); k=9; }
		   else { ps+=3; ptail+=3; set_wcs_end(endptr, ptail); k=4; }
	   }
   else if(towlower_macro (ps[0]) == L'n' && towlower_macro (ps[1]) == L'a' && 
	   towlower_macro (ps[2]) == L'n') {
		   ps+=3; ptail+=3;
		   while(iswdigit_macro(*ps)) {ps++; ptail++;}
		   set_wcs_end(endptr, ptail);
		   if(*ps0==L'-') {
		      ps0[0] = L'-';
			  ps0[1] = L'Q'; 
              ps0[2] = L'N'; 
              ps0[3] = L'A'; 
              ps0[4] = L'N'; 
              ps0[5] = L'\0'; 
              k=6; }
                           else {
              ps0[0] = L'Q'; 
              ps0[1] = L'N'; 
              ps0[2] = L'A'; 
              ps0[3] = L'N'; 
              ps0[4] = L'\0'; 
              k=5; }
	   }
   else {
	   k=1;
	   if(!iswdigit_macro(*ps) && ((*ps)!=L'.')) {
		   if(endptr) *endptr=(wchar_t*)ps_in;
		   free(ps0);
		   return NULL;  // no conversion
	   }
	   while(iswdigit_macro(*ps)) { ps++; ptail++; k++; }
	   if((*ps) == L'.') { 
		   if((ps0!=ps) || iswdigit_macro(ps[1])) {
			   ps++; ptail++; k++; while(iswdigit_macro(*ps)) { ps++; ptail++; k++; } }
		   else {
			   if(endptr) *endptr=(wchar_t*)ps_in;
			   free(ps0);
			   return NULL;  // no conversion
		   }
	   }
	   if(towlower_macro(*ps) == L'e') { 
		   if((ps[1]=='+') || (ps[1]=='-') || (iswdigit_macro(ps[1]))) { { ps+=2; ptail+=2; k+=2; }
		       while(iswdigit_macro(*ps)) { ps++; ptail++; k++; } }
	   }
		   set_wcs_end(endptr, ptail);
   }

   *ps = L'\0';
   ps0_c = malloc(k*sizeof(char));
   if(!ps0_c)
   { free(ps0); return NULL;}
   for(i=0; i<=k; i++)
	   ps0_c[i] = (ps0[i] - L'0') + '0';
   free(ps0);

   return ps0_c;

}


