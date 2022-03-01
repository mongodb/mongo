/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/******************************************************************************

This file provides a finite state machine to support Irish Gaelic uppercasing
rules.

The caller will need to iterate through a string, passing a State variable
along with the current character to each UpperCase call and checking the flags
that are returned:

  If aMarkPos is true, caller must remember the current index in the string as
  a possible target for a future action.

  If aAction is non-zero, then one or more characters from the marked index are
  to be modified:
    1  lowercase the marked letter
    2  lowercase the marked letter and its successor
    3  lowercase the marked letter, and delete its successor


### Rules from https://bugzilla.mozilla.org/show_bug.cgi?id=1014639,
### comments 1 and 4:

v = [a,á,e,é,i,í,o,ó,u,ú]
V = [A,Á,E,É,I,Í,O,Ó,U,Ú]

bhf -> bhF
bhF -> bhF
bp  -> bP
bP  -> bP
dt  -> dT
dT  -> dT
gc  -> gC
gC  -> gC
h{V}  -> h{V}
mb  -> mB
mB  -> mB
n-{v} -> n{V}
n{V} -> n{V}
nd  -> nD
nD  -> nD
ng  -> nG
nG  -> nG
t-{v} -> t{V}
t{V} -> t{V}
ts{v} -> tS{V}
tS{v} -> tS{V}
tS{V} -> tS{V}
tsl  -> tSL
tSl  -> tSL
tSL  -> tSL
tsn  -> tSN
tSn  -> tSN
tSN  -> tSN
tsr  -> tSR
tSr  -> tSR
tSR  -> tSR

### Create table of states and actions for each input class.

Start (non-word) state is #; generic in-word state is _, once we know there's
no special action to do in this word.

         #   _   b   bh  d   g   h   m   n   n-  t   t-  ts
input\state
b        b'  _   _   _   _   _   _   1   _   _   _   _   _
B        _   _   _   _   _   _   _   1   _   _   _   _   _
c        _   _   _   _   _   1   _   _   _   _   _   _   _
C        _   _   _   _   _   1   _   _   _   _   _   _   _
d        d'  _   _   _   _   _   _   _   1   _   _   _   _
D        _   _   _   _   _   _   _   _   1   _   _   _   _
f        _   _   _   2   _   _   _   _   _   _   _   _   _
F        _   _   _   2   _   _   _   _   _   _   _   _   _
g        g'  _   _   _   _   _   _   _   1   _   _   _   _
G        _   _   _   _   _   _   _   _   1   _   _   _   _
h        h'  _   bh  _   _   _   _   _   _   _   _   _   _
l        _   _   _   _   _   _   _   _   _   _   _   _   1
L        _   _   _   _   _   _   _   _   _   _   _   _   1
m        m'  _   _   _   _   _   _   _   _   _   _   _   _
n        n'  _   _   _   _   _   _   _   _   _   _   _   1
N        _   _   _   _   _   _   _   _   _   _   _   _   1
p        _   _   1   _   _   _   _   _   _   _   _   _   _
P        _   _   1   _   _   _   _   _   _   _   _   _   _
r        _   _   _   _   _   _   _   _   _   _   _   _   1
R        _   _   _   _   _   _   _   _   _   _   _   _   1
s        _   _   _   _   _   _   _   _   _   _   ts  _   _
S        _   _   _   _   _   _   _   _   _   _   ts  _   _
t        t'  _   _   _   1   _   _   _   _   _   _   _   _
T        _   _   _   _   1   _   _   _   _   _   _   _   _
vowel    _   _   _   _   _   _   _   _   _   1d  _   1d  1
Vowel    _   _   _   _   _   _   1   _   1   _   1   _   1
hyph     _   _   _   _   _   _   _   _   n-  _   t-  _   _
letter   _   _   _   _   _   _   _   _   _   _   _   _   _
other    #   #   #   #   #   #   #   #   #   #   #   #   #

Actions:
  1            lowercase one letter at start of word
  2            lowercase two letters at start of word
  1d           lowercase one letter at start of word, and delete next
               (and then go to state _, nothing further to do in this word)

else just go to the given state; suffix ' indicates mark start-of-word.

### Consolidate identical states and classes:

         0   1   2   3   4   5   6   7   8   9   A   B
         #   _   b   bh  d   g   h   m   n [nt]- t   ts
input\state
b        b'  _   _   _   _   _   _   1   _   _   _   _
B        _   _   _   _   _   _   _   1   _   _   _   _
[cC]     _   _   _   _   _   1   _   _   _   _   _   _
d        d'  _   _   _   _   _   _   _   1   _   _   _
[DG]     _   _   _   _   _   _   _   _   1   _   _   _
[fF]     _   _   _   2   _   _   _   _   _   _   _   _
g        g'  _   _   _   _   _   _   _   1   _   _   _
h        h'  _   bh  _   _   _   _   _   _   _   _   _
[lLNrR]  _   _   _   _   _   _   _   _   _   _   _   1
m        m'  _   _   _   _   _   _   _   _   _   _   _
n        n'  _   _   _   _   _   _   _   _   _   _   1
[pP]     _   _   1   _   _   _   _   _   _   _   _   _
[sS]     _   _   _   _   _   _   _   _   _   _   ts  _
t        t'  _   _   _   1   _   _   _   _   _   _   _
T        _   _   _   _   1   _   _   _   _   _   _   _
vowel    _   _   _   _   _   _   _   _   _   1d  _   1
Vowel    _   _   _   _   _   _   1   _   1   _   1   1
hyph     _   _   _   _   _   _   _   _ [nt-] _ [nt-] _
letter   _   _   _   _   _   _   _   _   _   _   _   _
other    #   #   #   #   #   #   #   #   #   #   #   #

So we have 20 input classes, and 12 states.

State table array will contain bytes that encode action and new state:

  0x80  -  bit flag: mark start-of-word position
  0x40  -  currently unused
  0x30  -  action mask: 4 values
           0x00  -  do nothing
           0x10  -  lowercase one letter
           0x20  -  lowercase two letters
           0x30  -  lowercase one, delete one
  0x0F  -  next-state mask
******************************************************************************/

#include "IrishCasing.h"

#include "nsUnicodeProperties.h"
#include "nsUnicharUtils.h"

namespace mozilla {

const uint8_t IrishCasing::sUppercaseStateTable[kNumClasses][kNumStates] = {
    //  #     _     b     bh    d     g     h     m     n     [nt]- t     ts
    {0x82, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x01, 0x01, 0x01,
     0x01},  // b
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x01, 0x01, 0x01,
     0x01},  // B
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x10, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01},  // [cC]
    {0x84, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x01, 0x01,
     0x01},  // d
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x01, 0x01,
     0x01},  // [DG]
    {0x01, 0x01, 0x01, 0x21, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01},  // [fF]
    {0x85, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x01, 0x01,
     0x01},  // g
    {0x86, 0x01, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01},  // h
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x11},  // [lLNrR]
    {0x87, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01},  // m
    {0x88, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x11},  // n
    {0x01, 0x01, 0x11, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01},  // [pP]
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x0B,
     0x01},  // [sS]
    {0x8A, 0x01, 0x01, 0x01, 0x11, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01},  // t
    {0x01, 0x01, 0x01, 0x01, 0x11, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01},  // T
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x31, 0x01,
     0x11},  // vowel
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x01, 0x11, 0x01, 0x11,
     0x11},  // Vowel
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x09, 0x01, 0x09,
     0x01},  // hyph
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
     0x01},  // letter
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00}  // other
};

#define HYPHEN 0x2010
#define NO_BREAK_HYPHEN 0x2011
#define a_ACUTE 0x00e1
#define e_ACUTE 0x00e9
#define i_ACUTE 0x00ed
#define o_ACUTE 0x00f3
#define u_ACUTE 0x00fa
#define A_ACUTE 0x00c1
#define E_ACUTE 0x00c9
#define I_ACUTE 0x00cd
#define O_ACUTE 0x00d3
#define U_ACUTE 0x00da

const uint8_t IrishCasing::sLcClasses[26] = {
    kClass_vowel,  kClass_b,      kClass_cC,     kClass_d,      kClass_vowel,
    kClass_fF,     kClass_g,      kClass_h,      kClass_vowel,  kClass_letter,
    kClass_letter, kClass_lLNrR,  kClass_m,      kClass_n,      kClass_vowel,
    kClass_pP,     kClass_letter, kClass_lLNrR,  kClass_sS,     kClass_t,
    kClass_vowel,  kClass_letter, kClass_letter, kClass_letter, kClass_letter,
    kClass_letter};

const uint8_t IrishCasing::sUcClasses[26] = {
    kClass_Vowel,  kClass_B,      kClass_cC,     kClass_DG,     kClass_Vowel,
    kClass_fF,     kClass_DG,     kClass_letter, kClass_Vowel,  kClass_letter,
    kClass_letter, kClass_lLNrR,  kClass_letter, kClass_lLNrR,  kClass_Vowel,
    kClass_pP,     kClass_letter, kClass_lLNrR,  kClass_sS,     kClass_T,
    kClass_Vowel,  kClass_letter, kClass_letter, kClass_letter, kClass_letter,
    kClass_letter};

uint8_t IrishCasing::GetClass(uint32_t aCh) {
  using mozilla::unicode::GetGenCategory;
  if (aCh >= 'a' && aCh <= 'z') {
    return sLcClasses[aCh - 'a'];
  } else if (aCh >= 'A' && aCh <= 'Z') {
    return sUcClasses[aCh - 'A'];
  } else if (GetGenCategory(aCh) == nsUGenCategory::kLetter) {
    if (aCh == a_ACUTE || aCh == e_ACUTE || aCh == i_ACUTE || aCh == o_ACUTE ||
        aCh == u_ACUTE) {
      return kClass_vowel;
    } else if (aCh == A_ACUTE || aCh == E_ACUTE || aCh == I_ACUTE ||
               aCh == O_ACUTE || aCh == U_ACUTE) {
      return kClass_Vowel;
    } else {
      return kClass_letter;
    }
  } else if (aCh == '-' || aCh == HYPHEN || aCh == NO_BREAK_HYPHEN) {
    return kClass_hyph;
  } else {
    return kClass_other;
  }
}

uint32_t IrishCasing::UpperCase(uint32_t aCh, State& aState, bool& aMarkPos,
                                uint8_t& aAction) {
  uint8_t cls = GetClass(aCh);
  uint8_t stateEntry = sUppercaseStateTable[cls][aState];
  aMarkPos = !!(stateEntry & kMarkPositionFlag);
  aAction = (stateEntry & kActionMask) >> kActionShift;
  aState = State(stateEntry & kNextStateMask);

  return ToUpperCase(aCh);
}

}  // namespace mozilla
