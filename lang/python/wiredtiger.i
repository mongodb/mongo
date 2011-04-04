%module wiredtiger

/* Set the input argument to point to a temporary variable */ 
%typemap(in, numinputs=0) WT_CONNECTION ** (WT_CONNECTION *temp = NULL) {
        $1 = &temp;
}
%typemap(in, numinputs=0) WT_SESSION ** (WT_SESSION *temp = NULL) {
        $1 = &temp;
}
%typemap(in, numinputs=0) WT_CURSOR ** (WT_CURSOR *temp = NULL) {
        $1 = &temp;
}

/* Set the return value to the returned connection, session, or cursor */
%typemap(argout) WT_CONNECTION ** {
        $result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
             SWIGTYPE_p_wt_connection, 0);
}
%typemap(argout) WT_SESSION ** {
        $result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
             SWIGTYPE_p_wt_session, 0);
}

%typemap(argout) WT_CURSOR ** {
        $result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
             SWIGTYPE_p_wt_cursor, 0);
}


/* Checking for error returns - any error is an exception.
 * TODO: need to create a wiredtiger exception, probably language specific
 */
%typemap(out) int {
	$result = SWIG_From_int((int)(result));
	if ($1 != 0 && $1 != WT_NOTFOUND) {
                SWIG_exception_fail(SWIG_RuntimeError, wiredtiger_strerror($1));
		return NULL;
        }
}

/*
 * Extra 'self' elimination.
 * The methods we're wrapping look like this:
 * struct wt_xxx {
 *    int method(WT_XXX *, ...otherargs...);
 * };
 * To SWIG, that is equivalent to:
 *    int method(wt_xxx *self, WT_XXX *, ...otherargs...);
 * and we use consecutive argument matching of typemaps to convert two args to one.
 */
%typemap(in) (struct wt_connection *self, WT_CONNECTION *) (void *argp = 0, int res = 0) %{
        res = SWIG_ConvertPtr($input, &argp, $descriptor, $disown | 0);
        if (!SWIG_IsOK(res)) { 
                SWIG_exception_fail(SWIG_ArgError(res), "in method '" "$symname" "', argument " "$argnum" " of type '" "$type" "'");
        }
        $2 = $1 = ($ltype)(argp);
%}

%typemap(in) (struct wt_session *self, WT_SESSION *) (void *argp = 0, int res = 0) %{
        res = SWIG_ConvertPtr($input, &argp, $descriptor, $disown | 0);
        if (!SWIG_IsOK(res)) { 
                SWIG_exception_fail(SWIG_ArgError(res), "in method '" "$symname" "', argument " "$argnum" " of type '" "$type" "'");
        }
        $2 = $1 = ($ltype)(argp);
%}

%typemap(in) (struct wt_cursor *self, WT_CURSOR *) (void *argp = 0, int res = 0) %{
        res = SWIG_ConvertPtr($input, &argp, $descriptor, $disown | 0);
        if (!SWIG_IsOK(res)) { 
                SWIG_exception_fail(SWIG_ArgError(res), "in method '" "$symname" "', argument " "$argnum" " of type '" "$type" "'");
        }
        $2 = $1 = ($ltype)(argp);
%}

/* WT_CURSOR customization.
 * We want our own 'next' function for wt_cursor, so we can
 * implement iterable.  Since nobody but use will use
 * the 'real' version, we'll remove it to avoid confusion.
 */
%ignore next(WT_CURSOR *);

/* We create an artificial return type for next, and use the typemap to
 * convert it to its final value (a python tuple).  That consolidates
 * the return code.
 * TODO: no need to do this anymore?
 */
%{
typedef struct IterNextValue {
        PyObject *pyobj;
        int ret;
} IterNextValue;
%}

%typemap(out) IterNextValue {
        if ($1.ret != 0) {
                if ($1.ret == WT_NOTFOUND) {
                        PyErr_SetString(PyExc_StopIteration, "No more data for cursor"); 
                }
                else {
                        PyErr_SetString(PyExc_RuntimeError, wiredtiger_strerror($1.ret));
                }
                $1.pyobj = NULL;
        }
        $result = $1.pyobj;
}

/* Implement the iterable contract for wt_cursor */
%extend wt_cursor {
        struct wt_cursor *__iter__() {
                return $self;
        }

	IterNextValue next() {
                // TODO: handle arbitrary types, not just strings!
                char *keyptr = 0;
        	char *valptr = 0;
                IterNextValue result;
                int ret;

                result.pyobj = NULL;
                ret = self->next(self);
#if 0
                //TODO: for testing until real insertion works
                {
                        // fake some values to test the caller...
                        static int count = 2;

                        if (count-- > 0) {
                                result.pyobj = Py_BuildValue("(ss)", "foo", "bar");
                                result.ret = 0;
                                return result;
                        }
                        else {
                                ret = EIO; /* TODO!! */
                        }
                }
#endif

                if (ret == 0) {
                        ret = self->get_key(self, &keyptr);
                }
                if (ret == 0) {
                        ret = self->get_value(self, &valptr);
                }
                if (ret == 0) {
                        result.pyobj = Py_BuildValue("(ss)", keyptr, valptr);
                }
                result.ret = ret;
                return result;
        }
};

/* TODO:
// This typemap handles set_key, set_value.
// Unfortunately, the only way to distinguish between these two
// calls is to look at $symname.
%typemap(in) (WT_CURSOR *, ...) {
        int symlen = strlen($symname);
        int iskey = (symname == 7);
}
*?

/*TODO: %varargs(void *arg) wt_cursor.set_key;*/
%varargs(char *arg) set_key;
%varargs(char *arg) set_value;

%include "wiredtiger.h"
