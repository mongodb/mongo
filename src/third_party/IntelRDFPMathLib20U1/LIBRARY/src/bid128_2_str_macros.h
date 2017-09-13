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

#define __L0_Normalize_10to18( X_hi, X_lo )            \
{                                                      \
BID_UINT64 L0_tmp;                                         \
L0_tmp = (X_lo) + bid_Twoto60_m_10to18;                    \
if (L0_tmp & bid_Twoto60)                                  \
 {(X_hi)=(X_hi)+1;(X_lo)=((L0_tmp<<4)>>4);}            \
}


#define __L0_Normalize_10to9( X_hi, X_lo )             \
{                                                      \
BID_UINT32 L0_tmp;                                         \
L0_tmp = (X_lo) + bid_Twoto30_m_10to9;                     \
if (L0_tmp & 0x40000000)                               \
 {(X_hi)=(X_hi)+1;(X_lo)=((L0_tmp<<2)>>2);}            \
}


#define __L0_Split_MiDi_2( X, ptr )                    \
{                                                      \
BID_UINT32 L0_head, L0_tail, L0_tmp;                       \
 L0_head = (X) >> 10;                                  \
 L0_tail = ((X)&(0x03FF))+(L0_head<<5)-(L0_head<<3);   \
 L0_tmp  = (L0_tail)>>10; L0_head += L0_tmp;           \
 L0_tail = (L0_tail&(0x03FF))+(L0_tmp<<5)-(L0_tmp<<3); \
 if (L0_tail > 999){L0_tail -= 1000; L0_head += 1;}    \
 *((ptr)++) = L0_head; *((ptr)++) = L0_tail;           \
}


#define __L0_Split_MiDi_3( X, ptr )                    \
{                                                      \
BID_UINT32 L0_X, L0_head, L0_mid, L0_tail, L0_tmp;         \
 L0_X    = (BID_UINT32)((X));                              \
 L0_head = ((L0_X>>17)*34359)>>18;                     \
 L0_X   -= L0_head*1000000;                            \
 if (L0_X >= 1000000){L0_X -= 1000000;L0_head+=1;}     \
 L0_mid  = L0_X >> 10;                                 \
 L0_tail = (L0_X & (0x03FF))+(L0_mid<<5)-(L0_mid<<3);  \
 L0_tmp  = (L0_tail)>>10; L0_mid += L0_tmp;            \
 L0_tail = (L0_tail&(0x3FF))+(L0_tmp<<5)-(L0_tmp<<3);  \
 if (L0_tail>999){L0_tail-=1000;L0_mid+=1;}            \
 *((ptr)++)=L0_head;*((ptr)++)=L0_mid;                 \
 *((ptr)++)=L0_tail;                                   \
}

#define __L1_Split_MiDi_6( X, ptr )                    \
{                                                      \
BID_UINT32 L1_X_hi, L1_X_lo;                               \
BID_UINT64 L1_Xhi_64, L1_Xlo_64;                           \
L1_Xhi_64 = ( ((X)>>28)*bid_Inv_Tento9 ) >> 33;            \
L1_Xlo_64 = (X) - L1_Xhi_64*(BID_UINT64)bid_Tento9;            \
if (L1_Xlo_64 >= (BID_UINT64)bid_Tento9)                       \
 {L1_Xlo_64-=(BID_UINT64)bid_Tento9;L1_Xhi_64+=1;}             \
L1_X_hi=(BID_UINT32)L1_Xhi_64; L1_X_lo=(BID_UINT32)L1_Xlo_64;  \
__L0_Split_MiDi_3(L1_X_hi,(ptr));                      \
__L0_Split_MiDi_3(L1_X_lo,(ptr));                      \
}

#define __L1_Split_MiDi_6_Lead( X, ptr )               \
{                                                      \
BID_UINT32 L1_X_hi, L1_X_lo;                               \
BID_UINT64 L1_Xhi_64, L1_Xlo_64;                           \
if ((X)>=(BID_UINT64)bid_Tento9){                              \
  L1_Xhi_64 = ( ((X)>>28)*bid_Inv_Tento9 ) >> 33;          \
  L1_Xlo_64 = (X) - L1_Xhi_64*(BID_UINT64)bid_Tento9;          \
  if (L1_Xlo_64 >= (BID_UINT64)bid_Tento9)                     \
   {L1_Xlo_64-=(BID_UINT64)bid_Tento9;L1_Xhi_64+=1;}           \
  L1_X_hi=(BID_UINT32)L1_Xhi_64;                           \
  L1_X_lo=(BID_UINT32)L1_Xlo_64;                           \
  if (L1_X_hi>=bid_Tento6){                                \
     __L0_Split_MiDi_3(L1_X_hi,(ptr));                 \
     __L0_Split_MiDi_3(L1_X_lo,(ptr));                 \
  }                                                    \
  else if (L1_X_hi>=bid_Tento3){                           \
     __L0_Split_MiDi_2(L1_X_hi,(ptr));                 \
     __L0_Split_MiDi_3(L1_X_lo,(ptr));                 \
  }                                                    \
  else {                                               \
       *((ptr)++) = L1_X_hi;                           \
       __L0_Split_MiDi_3(L1_X_lo,(ptr));               \
  }                                                    \
}                                                      \
else {                                                 \
  L1_X_lo = (BID_UINT32)(X);                               \
  if (L1_X_lo>=bid_Tento6){                                \
     __L0_Split_MiDi_3(L1_X_lo,(ptr));                 \
  }                                                    \
  else if (L1_X_lo>=bid_Tento3){                           \
     __L0_Split_MiDi_2(L1_X_lo,(ptr));                 \
  }                                                    \
  else {                                               \
       *((ptr)++) = L1_X_lo;                           \
  }                                                    \
}                                                      \
}


#define __L0_MiDi2Str( X, c_ptr )              \
{                                              \
const char *L0_src;                            \
 L0_src = bid_midi_tbl[(X)];                       \
 *((c_ptr)++) = *(L0_src++);                   \
 *((c_ptr)++) = *(L0_src++);                   \
 *((c_ptr)++) = *(L0_src);                     \
}

#define __L0_MiDi2Str_Lead( X, c_ptr )         \
{                                              \
const char *L0_src;                            \
 L0_src = bid_midi_tbl[(X)];                       \
 if ((X)>=100){                                \
 *((c_ptr)++) = *(L0_src++);                   \
 *((c_ptr)++) = *(L0_src++);                   \
 *((c_ptr)++) = *(L0_src);                     \
 }                                             \
 else if ((X)>=10){                            \
 L0_src++;                                     \
 *((c_ptr)++) = *(L0_src++);                   \
 *((c_ptr)++) = *(L0_src);                     \
 }                                             \
 else {                                        \
 L0_src++;L0_src++;                            \
 *((c_ptr)++) = *(L0_src);                     \
}                                              \
}
