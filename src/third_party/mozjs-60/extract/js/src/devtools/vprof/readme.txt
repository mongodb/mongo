# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

The two files vprof.h and vprof.cpp implement a simple value-profiling mechanism. By including these two files in avmplus (or any other project), you can value profile data as you wish (currently integers).

Usage:
#include "vprof.h"  // in the source file you want to use it

_vprof (value);

At the end of the execution, for each probe you'll get the data associated with the probe, such as:

File                                        line    avg     [min : max] total       count
..\..\pcre\pcre_valid_utf8.cpp  182 50222.75916 [0 :    104947] 4036955604  80381

The probe is defined at line 182 of file pcre_vali_utf8.cpp. It was called 80381 times. The min value of the probe was 0 while its max was 10497 and its average was  50222.75916. The total sum of all values of the probe is 4036955604. Later, I plan to add more options on the spectrum of data among others.

A few typical uses
------------------

To see how many times a given function gets executed do:

void f()
{
    _vprof(1);
    ...
}

void f()
{
        _vprof(1);
        ...
       if (...) {
           _vprof(1);
           ...
       } else {
           _vprof(1);
           ...
       }
}

Here are a few examples of using the value-profiling utility:

  _vprof (e);
    at the end of program execution, you'll get a dump of the source location of this probe,
    its min, max, average, the total sum of all instances of e, and the total number of times this probe was called.

  _vprof (x > 0);
    shows how many times and what percentage of the cases x was > 0,
    that is the probablitiy that x > 0.

 _vprof (n % 2 == 0);
    shows how many times n was an even number
    as well as th probablitiy of n being an even number.

 _hprof (n, 4, 1000, 5000, 5001, 10000);
    gives you the histogram of n over the given 4 bucket boundaries:
        # cases <  1000
        # cases >= 1000 and < 5000
        # cases >= 5000 and < 5001
        # cases >= 5001 and < 10000
        # cases >= 10000

 _nvprof ("event name", value);
    all instances with the same name are merged
    so, you can call _vprof with the same event name at difference places

 _vprof (e, myProbe);
    value profile e and call myProbe (void* vprofID) at the profiling point.
    inside the probe, the client has the predefined variables:
    _VAL, _COUNT, _SUM, _MIN, _MAX, and the general purpose registers
    _IVAR1, ..., IVAR4      general integer registrs
    _I64VAR1, ..., I64VAR4  general integer64 registrs
    _DVAR1, ..., _DVAR4     general double registers
    _GENPTR a generic pointer that can be used by the client
    the number of registers can be changed in vprof.h

Named Events
------------
_nvprof ("event name", value);
    all instances with the same name are merged
    so, you can call _vprof with the same event name at difference places


Custom Probes
--------------
You can call your own custom probe at the profiling point.
_vprof (v, myProbe);
   value profile v and call myProbe (void* vprofID) at the profiling point
   inside the probe, the client has the predefined variables:
   _VAL, _COUNT, _SUM, _MIN, _MAX, and the general purpose registers
   _IVAR1, ..., IVAR4   general integer registrs
   _I64VAR1, ..., I64VAR4 general integer64 registrs
   _DVAR1, ..., _DVAR4  general double registers
  the number of registers can be changed in vprof.h
  _GENPTR a generic pointer that can be used for almost anything
