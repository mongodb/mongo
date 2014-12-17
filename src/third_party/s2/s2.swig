// Copyright 2006 Google Inc. All Rights Reserved.
// Author: Andrew Eland (andrewe@google.com)

%include base/google.swig

%{
#include <strstream>

#include "util/geometry/s2cellid.h"
#include "util/geometry/s2region.h"
#include "util/geometry/s2cap.h"
#include "util/geometry/s2latlng.h"
#include "util/geometry/s2latlngrect.h"
#include "util/geometry/s2regioncoverer.h"
#include "util/geometry/s2cell.h"
#include "util/geometry/s2cellunion.h"
%}

// The PACKED macro makes SWIG think that we're declaring a variable of type
// S2CellId named PACKED.  We don't need it so we clobber it with an empty body.
#define PACKED

// The DECLARE_POD macro makes SWIG think the specified type's defined twice.
// We don't need it, so we can just remove it.
#define DECLARE_POD(TypeName)

// Warning 510 is "friend function 'operator +' ignored.". We can't do anything
// about that.
#pragma SWIG nowarn=510

// If we don't ignore this, the wrapper ends up assigning to None
%ignore S2CellId::None;

#ifdef SWIGPYTHON

%inline %{
  static PyObject *FromS2CellId(const S2CellId &cell_id) {
    return SWIG_NewPointerObj(new S2CellId(cell_id), SWIGTYPE_p_S2CellId, 1);
  }
%}

%typemap(in, numinputs=0)
vector<S2CellId> *OUTPUT(vector<S2CellId> temp) {
  $1 = &temp;
}

%typemap(argout, fragment="t_output_helper")
vector<S2CellId> *OUTPUT {
  $result = t_output_helper($result, vector_output_helper($1, &FromS2CellId));
}

%apply vector<S2CellId> *OUTPUT {vector<S2CellId> *covering};
%apply vector<S2CellId> *OUTPUT {vector<S2CellId> *output};

#endif

%include "util/geometry/r1interval.h"
%include "util/geometry/s1angle.h"
%include "util/geometry/s1interval.h"
%include "util/geometry/s2cellid.h"
%include "util/geometry/s2region.h"
%include "util/geometry/s2cap.h"
%include "util/geometry/s2latlng.h"
%include "util/geometry/s2latlngrect.h"
%include "util/geometry/s2regioncoverer.h"
%include "util/geometry/s2cell.h"
%include "util/geometry/s2cellunion.h"

%define USE_STREAM_INSERTOR_FOR_STR(type)
  %extend type {
    string __str__() {
      ostrstream output;
      output << *self << std::ends;
      return output.str();
    }
  }
%enddef

%define USE_EQUALS_FOR_EQ_AND_NE(type)
  %extend type {
    bool __eq__(const type& other) {
      return *self == other;
    }

    bool __ne__(const type& other) {
      return *self != other;
    }
  }
%enddef

%define USE_COMPARISON_FOR_LT_AND_GT(type)
  %extend type {
    bool __lt__(const type& other) {
      return *self < other;
    }

    bool __gt__(const type& other) {
      return *self > other;
    }
  }
%enddef

%define USE_STD_HASH_FOR_HASH(type)
  %extend type {
    size_t __hash__() {
      return HASH_NAMESPACE::hash<type>()(*self);
    }
  }
%enddef

USE_STREAM_INSERTOR_FOR_STR(S1Angle)
USE_STREAM_INSERTOR_FOR_STR(S1Interval)
USE_STREAM_INSERTOR_FOR_STR(S2CellId)
USE_STREAM_INSERTOR_FOR_STR(S2Cap)
USE_STREAM_INSERTOR_FOR_STR(S2LatLng)
USE_STREAM_INSERTOR_FOR_STR(S2LatLngRect)

USE_EQUALS_FOR_EQ_AND_NE(S2CellId)
USE_COMPARISON_FOR_LT_AND_GT(S2CellId)
USE_STD_HASH_FOR_HASH(S2CellId)
