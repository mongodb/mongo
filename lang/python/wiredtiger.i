%module wiredtiger

/* Set the input argument to point to a temporary variable */ 
%typemap(in, numinputs=0) WT_CONNECTION ** (WT_CONNECTION *temp) {
        $1 = &temp;
}
/* Append connection to returned list */
%typemap(argout) WT_CONNECTION ** {
        $result = SWIG_NewPointerObj(SWIG_as_voidptr($1),
             SWIGTYPE_p_wt_connection, 0);
}

%include "wiredtiger.h"
