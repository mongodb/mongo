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
 * as expected SWIG creates:
 *    wt_xxx.method(WT_XXX *, ...otherargs...);
 * so we'd like to eliminate the WT_XXX * and set its value to
 * the wt_xxx var.  To make life difficult, a typemap with numinputs=0
 * (which we want to use for WT_XXX *) seems to be emitted before
 * all other typemaps.  SWIG doc claims that adjacent args can be
 * typemapped as a single unit, that doesn't work here, I guess these two
 * args are apparently not considered adjacent.  Furthermore, 
 * there's no way to create an extra stack variable for them
 * to communicate.
 *
 * Our solution is to use argp1 (a var that will be used used by
 * wt_xxx *) as a holder.  The holder keeps the address where we want
 * stored the computed self value.  The 'in' typemap for WT_XXX * sets
 * that, and the 'in' typemape for wt_xxx * uses it to store the
 * value.  prepare_copy_self() and copy_self() encapsulate this
 * communication between the two typemaps.
 */
%{
        static void prepare_copy_self(void ***holderp, void **myargp)
        {
                if (*holderp == NULL) {
                        *holderp = myargp;
                }
        }

        static void copy_self(void **holder, void *self)
        {
                if (holder != NULL) {
                        *holder = self;
                }
        }
%}

// Can't do:  $1 = self; , when this typemap is used, we don't always have
// self??  TODO: look at this more, it would clean things up some.
%typemap(in, numinputs=0) WT_CONNECTION *thisconnection, WT_SESSION *thissession, WT_CURSOR *thiscursor %{
        prepare_copy_self((void ***)&argp1, (void **)&$1);
%}

// For some reason the following three cannot be combined in a single typemap
%typemap(in) struct wt_connection *self (void *argp = 0, int res = 0, void **copy) {
        // struct wt_connection *
        copy = (void **)argp;
        res = SWIG_ConvertPtr($input, &argp,$descriptor, $disown | 0);
        if (!SWIG_IsOK(res)) { 
                SWIG_exception_fail(SWIG_ArgError(res), "in method '" "$symname" "', argument " "$argnum" " of type '" "$type" "'");
        }
        $1 = ($ltype)(argp);
        copy_self(copy, $1);
}

%typemap(in) struct wt_session *self (void *argp = 0, int res = 0, void **copy) {
        // struct wt_session *
        copy = (void **)argp;
        res = SWIG_ConvertPtr($input, &argp,$descriptor, $disown | 0);
        if (!SWIG_IsOK(res)) { 
                SWIG_exception_fail(SWIG_ArgError(res), "in method '" "$symname" "', argument " "$argnum" " of type '" "$type" "'");
        }
        $1 = ($ltype)(argp);
        copy_self(copy, $1);
}

%typemap(in) struct wt_cursor *self (void *argp = 0, int res = 0, void **copy) {
        // struct wt_cursor *
        copy = (void **)argp;
        res = SWIG_ConvertPtr($input, &argp,$descriptor, $disown | 0);
        if (!SWIG_IsOK(res)) { 
                SWIG_exception_fail(SWIG_ArgError(res), "in method '" "$symname" "', argument " "$argnum" " of type '" "$type" "'");
        }
        $1 = ($ltype)(argp);
        copy_self(copy, $1);
}

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
