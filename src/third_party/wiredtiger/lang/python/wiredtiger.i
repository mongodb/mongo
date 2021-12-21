/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * wiredtiger.i
 *	The SWIG interface file defining the wiredtiger python API.
 */
%include <pybuffer.i>

%define DOCSTRING
"Python wrappers around the WiredTiger C API

This provides an API similar to the C API, with the following modifications:
  - Many C functions are exposed as OO methods. See the Python examples and test suite
  - Errors are handled in a Pythonic way; wrap calls in try/except blocks
  - Cursors have extra accessor methods and iterators that are higher-level than the C API
  - Statistics cursors behave a little differently and are best handled using the C-like functions
  - C Constants starting with WT_STAT_DSRC are instead exposed under wiredtiger.stat.dsrc
  - C Constants starting with WT_STAT_CONN are instead exposed under wiredtiger.stat.conn
  - C Constants starting with WT_STAT_SESSION are instead exposed under wiredtiger.stat.session
"
%enddef

%module(docstring=DOCSTRING) wiredtiger

%feature("autodoc", "0");

%pythoncode %{
from packing import pack, unpack
## @endcond
%}

/*
 * For some reason, SWIG doesn't import some types from stdint.h.  We need to tell SWIG something
 * about those type we use.  We don't need to be exact in our typing here, SWIG just needs hints
 * so it knows what Python types to map to.
 */
%inline %{
    typedef unsigned int uint32_t;
%}

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
%typemap(in, numinputs=0) WT_FILE_HANDLE ** (WT_FILE_HANDLE *temp = NULL) {
    $1 = &temp;
 }
%typemap(in, numinputs=0) WT_FILE_SYSTEM ** (WT_FILE_SYSTEM *temp = NULL) {
    $1 = &temp;
 }
%typemap(in, numinputs=0) WT_STORAGE_SOURCE ** (WT_STORAGE_SOURCE *temp = NULL) {
	$1 = &temp;
 }
%typemap(in, numinputs=0) bool * (bool temp = false) {
	$1 = &temp;
 }
%typemap(in, numinputs=0) wt_off_t * (wt_off_t temp = false) {
	$1 = &temp;
}

%typemap(in, numinputs=0) WT_EVENT_HANDLER * %{
	$1 = &pyApiEventHandler;
%}

/* Set the return value to the returned connection, session, or cursor */
%typemap(argout) WT_CONNECTION ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
	    SWIGTYPE_p___wt_connection, 0);
}
%typemap(argout) WT_SESSION ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
	    SWIGTYPE_p___wt_session, 0);
	if (*$1 != NULL) {
		PY_CALLBACK *pcb;

		if (__wt_calloc_def((WT_SESSION_IMPL *)(*$1), 1, &pcb) != 0)
			SWIG_exception_fail(SWIG_MemoryError, "WT calloc failed");
		else {
			Py_XINCREF($result);
			pcb->pyobj = $result;
			((WT_SESSION_IMPL *)(*$1))->lang_private = pcb;
		}
	}
}
%typemap(argout) WT_CURSOR ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
	    SWIGTYPE_p___wt_cursor, 0);
	if (*$1 != NULL) {
		PY_CALLBACK *pcb;
		uint32_t json;

		json = (*$1)->flags & WT_CURSTD_DUMP_JSON;
		if (!json)
			(*$1)->flags |= WT_CURSTD_RAW;
		PyObject_SetAttrString($result, "is_json",
		    PyBool_FromLong(json != 0));
		PyObject_SetAttrString($result, "is_column",
		    PyBool_FromLong(strcmp((*$1)->key_format, "r") == 0));
		PyObject_SetAttrString($result, "key_format",
		    PyString_InternFromString((*$1)->key_format));
		PyObject_SetAttrString($result, "value_format",
		    PyString_InternFromString((*$1)->value_format));

		if (__wt_calloc_def((WT_SESSION_IMPL *)(*$1)->session, 1, &pcb) != 0)
			SWIG_exception_fail(SWIG_MemoryError, "WT calloc failed");
		else {
			Py_XINCREF($result);
			pcb->pyobj = $result;
			(*$1)->lang_private = pcb;
		}
	}
}

%typemap(in,numinputs=1) (WT_MODIFY *entries, int *nentriesp) (WT_MODIFY *mod, int nentries) {
	nentries = (int) PyLong_AsLong($input);
	if (__wt_calloc_def(NULL, (size_t)nentries, &mod) != 0)
		SWIG_exception_fail(SWIG_MemoryError, "WT calloc failed");
	$1 = mod;
	$2 = &nentries;
}

%typemap(argout) (WT_MODIFY *entries, int *nentriesp) {
	int i;

	$result = PyList_New(*$2);
	for (i = 0; i < *$2; i++) {
		PyObject *o = SWIG_NewPointerObj(Py_None, SWIGTYPE_p___wt_modify, 0);
		PyObject_SetAttrString(o, "data", PyBytes_FromStringAndSize(
		    $1[i].data.data, $1[i].data.size));
		PyObject_SetAttrString(o, "offset",
		    PyInt_FromLong($1[i].offset));
		PyObject_SetAttrString(o, "size",
		    PyInt_FromLong($1[i].size));
		PyList_SetItem($result, i, o);
	}
}

%typemap(argout) (WT_MODIFY *entries_string, int *nentriesp) {
	int i;

	$result = PyList_New(*$2);
	for (i = 0; i < *$2; i++) {
		PyObject *o = SWIG_NewPointerObj(Py_None, SWIGTYPE_p___wt_modify, 0);
		PyObject_SetAttrString(o, "data", PyUnicode_FromStringAndSize(
		    $1[i].data.data, $1[i].data.size));
		PyObject_SetAttrString(o, "offset",
		    PyInt_FromLong($1[i].offset));
		PyObject_SetAttrString(o, "size",
		    PyInt_FromLong($1[i].size));
		PyList_SetItem($result, i, o);
	}
 }

%typemap(in) const WT_ITEM * (WT_ITEM val) {
	if (unpackBytesOrString($input, &val.data, &val.size) != 0)
		SWIG_exception_fail(SWIG_AttributeError,
		    "bad string value for WT_ITEM");
	$1 = &val;
}

%typemap(in,numinputs=0) (char ***dirlist, int *countp) (char **list, uint32_t nentries) {
	$1 = &list;
	$2 = &nentries;
}

%typemap(argout) (char ***dirlist, int *countp) {
	int i;
	char **list;

	$result = PyList_New(*$2);
	list = (*$1);
	/*
	 * When we're done with the individual C strings, free them.
	 * In theory, we should call the fs_directory_list_free() method,
	 * but that's awkward, since we don't have the file system and session.
	 */
	for (i = 0; i < *$2; i++) {
		PyObject *o = PyString_InternFromString(list[i]);
		PyList_SetItem($result, i, o);
		free(list[i]);
	}
	free(list);
}


%typemap(argout) WT_FILE_HANDLE ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1), SWIGTYPE_p___wt_file_handle, 0);
}

%typemap(argout) WT_FILE_SYSTEM ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1), SWIGTYPE_p___wt_file_system, 0);
}

%typemap(argout) WT_STORAGE_SOURCE ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1), SWIGTYPE_p___wt_storage_source, 0);
}

%typemap(argout) bool * {
	$result = PyBool_FromLong(*$1);
}

%typemap(argout) wt_off_t * {
	$result = PyInt_FromLong(*$1);
}

%typemap(freearg) (WT_MODIFY *, int *nentriesp) {
	__wt_free(NULL, $1);
}

%typemap(in) WT_MODIFY * (int len, WT_MODIFY *modarray, int i) {
	len = PyList_Size($input);
	/*
	 * We allocate an extra cleared WT_MODIFY struct, the first
	 * entry will be used solely to transmit the array length to
	 * the call site.
	 */
	if (__wt_calloc_def(NULL, (size_t)len + 1, &modarray) != 0)
		SWIG_exception_fail(SWIG_MemoryError, "WT calloc failed");
	modarray[0].size = (size_t)len;
	for (i = 1; i <= len; i++) {
		PyObject *dataobj, *modobj, *offsetobj, *sizeobj;
		void *datadata;
		long offset, size;
		size_t datasize;

		if ((modobj = PySequence_GetItem($input, i - 1)) == NULL) {
			freeModifyArray(modarray);
			SWIG_exception_fail(SWIG_IndexError,
			    "Modify sequence failed");
		}

		WT_GETATTR(dataobj, modobj, "data");
		if (unpackBytesOrString(dataobj, &datadata, &datasize) != 0) {
			Py_DECREF(dataobj);
			Py_DECREF(modobj);
			freeModifyArray(modarray);
			SWIG_exception_fail(SWIG_AttributeError,
			    "Modify.data bad value");
		}
		if (datasize != 0 &&
		    __wt_malloc(NULL, datasize, &modarray[i].data.data) != 0) {
			Py_DECREF(dataobj);
			Py_DECREF(modobj);
			freeModifyArray(modarray);
			SWIG_exception_fail(SWIG_AttributeError,
			    "Modify.data failed malloc");
		}
		memcpy(modarray[i].data.data, datadata, datasize);
		modarray[i].data.size = datasize;
		Py_DECREF(dataobj);

		WT_GETATTR(offsetobj, modobj, "offset");
		if ((offset = PyInt_AsLong(offsetobj)) < 0) {
			Py_DECREF(offsetobj);
			Py_DECREF(modobj);
			freeModifyArray(modarray);
			SWIG_exception_fail(SWIG_RuntimeError,
			    "Modify.offset bad value");
		}
		modarray[i].offset = offset;
		Py_DECREF(offsetobj);

		WT_GETATTR(sizeobj, modobj, "size");
		if ((size = PyInt_AsLong(sizeobj)) < 0) {
			Py_DECREF(sizeobj);
			Py_DECREF(modobj);
			freeModifyArray(modarray);
			SWIG_exception_fail(SWIG_RuntimeError,
			    "Modify.size bad value");
		}
		modarray[i].size = size;
		Py_DECREF(sizeobj);
		Py_DECREF(modobj);
	}
	$1 = modarray;
}

%typemap(freearg) WT_MODIFY * {
	freeModifyArray($1);
}

/* 64 bit typemaps. */
%typemap(in) uint64_t {
	$1 = PyLong_AsUnsignedLongLong($input);
}
%typemap(out) uint64_t {
	$result = PyLong_FromUnsignedLongLong($1);
}

/* Internal _set_key, _set_value methods take a 'bytes' object as parameter. */
%pybuffer_binary(unsigned char *data, int);

/* Throw away references after close. */
%define DESTRUCTOR(class, method)
%feature("shadow") class::method %{
	def method(self, *args):
		'''method(self, config) -> int
		
		@copydoc class::method'''
		try:
			self._freecb()
			return $action(self, *args)
		finally:
			self.this = None
%}
%enddef
DESTRUCTOR(__wt_connection, close)
DESTRUCTOR(__wt_cursor, close)
DESTRUCTOR(__wt_file_handle, close)
DESTRUCTOR(__wt_session, close)
DESTRUCTOR(__wt_storage_source, ss_terminate)
DESTRUCTOR(__wt_file_system, fs_terminate)

/*
 * OVERRIDE_METHOD must be used when overriding or extending an existing
 * method in the C interface.  It creates Python method() that calls
 * _method(), which is the extended version of the method.  This works
 * around potential naming conflicts.  Without this technique, for example,
 * defining __wt_cursor::equals() creates the wrapper function
 * __wt_cursor_equals(), which may be defined in the WT library.
 */
%define OVERRIDE_METHOD(cclass, pyclass, method, pyargs)
%extend cclass {
%pythoncode %{
	def method(self, *args):
		'''method pyargs -> int

		@copydoc class::method'''
		return self._##method(*args)
%}
};
%enddef

/* Don't require empty config strings. */
%typemap(default) const char *config { $1 = NULL; }
%typemap(default) WT_CURSOR *to_dup { $1 = NULL; }

/* 
 * Error returns other than WT_NOTFOUND generate an exception.
 * Use our own exception type, in future tailored to the kind
 * of error.
 */
%header %{

#include "src/include/wt_internal.h"

/*
 * Closed handle checking:
 *
 * The typedef WT_CURSOR_NULLABLE used in wiredtiger.h is only made
 * visible to the SWIG parser and is used to identify arguments of
 * Cursor type that are permitted to be null.  Likewise, typedefs
 * WT_{CURSOR,SESSION,CONNECTION}_CLOSED identify 'close' calls that
 * need explicit nulling of the swigCPtr.  We do not match the *_CLOSED
 * typedefs in Python SWIG, as we already have special cased 'close' methods.
 *
 * We want SWIG to see these 'fake' typenames, but not the compiler.
 */
#define WT_CURSOR_NULLABLE		WT_CURSOR
#define WT_CURSOR_CLOSED		WT_CURSOR
#define WT_SESSION_CLOSED		WT_SESSION
#define WT_CONNECTION_CLOSED		WT_CONNECTION

/*
 * For Connections, Sessions and Cursors created in Python, each of
 * WT_CONNECTION_IMPL, WT_SESSION_IMPL and WT_CURSOR have a
 * lang_private field that store a pointer to a PY_CALLBACK, alloced
 * during the various open calls.  {conn,session,cursor}CloseHandler()
 * functions reach into the associated Python object, set the 'this'
 * asttribute to None, and free the PY_CALLBACK.
 */
typedef struct {
	PyObject *pyobj;	/* the python Session/Cursor object */
} PY_CALLBACK;

static PyObject *wtError;

static int sessionFreeHandler(WT_SESSION *session_arg);
static int cursorFreeHandler(WT_CURSOR *cursor_arg);
static int unpackBytesOrString(PyObject *obj, void **data, size_t *size);

#define WT_GETATTR(var, parent, name)					\
	do if ((var = PyObject_GetAttrString(parent, name)) == NULL) {	\
		Py_DECREF(parent);					\
		SWIG_exception_fail(SWIG_AttributeError,		\
		    "Modify." #name " get failed");			\
	} while(0)
%}

%init %{
	/*
	 * Create an exception type and put it into the _wiredtiger module.
	 * First increment the reference count because PyModule_AddObject
	 * decrements it.  Then note that "m" is the local variable for the
	 * module in the SWIG generated code.  If there is a SWIG variable for
	 * this, I haven't found it.
	 */
	wtError = PyErr_NewException("_wiredtiger.WiredTigerError", NULL, NULL);
	Py_INCREF(wtError);
	PyModule_AddObject(m, "WiredTigerError", wtError);
%}

%pythoncode %{
WiredTigerError = _wiredtiger.WiredTigerError

# Python3 has no explicit long type, recnos work as ints
import sys
if sys.version_info >= (3, 0, 0):
	def _wt_recno(i):
		return i
else:
	def _wt_recno(i):
		return long(i)

## @cond DISABLE
# Implements the iterable contract
class IterableCursor:
	def __init__(self, cursor):
		self.cursor = cursor

	def __iter__(self):
		return self

	def __next__(self):
		if self.cursor.next() == WT_NOTFOUND:
			raise StopIteration
		return self.cursor.get_keys() + self.cursor.get_values()

	def next(self):
		return self.__next__()
## @endcond

def wiredtiger_calc_modify(session, oldv, newv, maxdiff, nmod):
	return _wiredtiger_calc_modify(session, oldv, newv, maxdiff, nmod)

def wiredtiger_calc_modify_string(session, oldv, newv, maxdiff, nmod):
	return _wiredtiger_calc_modify_string(session, oldv, newv, maxdiff, nmod)

%}

/* Bail out if arg or arg.this is None, else set res to the C pointer. */
%define CONVERT_WITH_NULLCHECK(argp, res)
	if ($input == Py_None) {
		SWIG_exception_fail(SWIG_NullReferenceError,
		    "in method '$symname', "
		    "argument $argnum of type '$type' is None");
	} else {
		res = SWIG_ConvertPtr($input, &argp, $descriptor, $disown | 0);
		if (!SWIG_IsOK(res)) {
			if (SWIG_Python_GetSwigThis($input) == 0) {
				SWIG_exception_fail(SWIG_NullReferenceError,
				    "in method '$symname', "
				    "argument $argnum of type '$type' is None");
			} else {
				SWIG_exception_fail(SWIG_ArgError(res),
				    "in method '$symname', "
				    "argument $argnum of type '$type'");
			}
		}
	}
%enddef

/*
 * Extra 'self' elimination.
 * The methods we're wrapping look like this:
 * struct __wt_xxx {
 *	int method(WT_XXX *, ...otherargs...);
 * };
 * To SWIG, that is equivalent to:
 *	int method(struct __wt_xxx *self, WT_XXX *, ...otherargs...);
 * and we use consecutive argument matching of typemaps to convert two args to
 * one.
 */
%define SELFHELPER(type, name)
%typemap(in) (type *self, type *name) (void *argp = 0, int res = 0) %{
	CONVERT_WITH_NULLCHECK(argp, res)
	$2 = $1 = ($ltype)(argp);
%}
%typemap(in) type ## _NULLABLE * {
	$1 = *(type **)&$input;
}

%enddef

SELFHELPER(struct __wt_connection, connection)
SELFHELPER(struct __wt_session, session)
SELFHELPER(struct __wt_cursor, cursor)
SELFHELPER(struct __wt_file_handle, file_handle)
SELFHELPER(struct __wt_file_system, file_system)
SELFHELPER(struct __wt_storage_source, storage_source)

 /*
  * Create an error exception if it has not already
  * been done.
  */
%define SWIG_ERROR_IF_NOT_SET(result)
do {
	if (PyErr_Occurred() == NULL) {
		/* We could use PyErr_SetObject for more complex reporting. */
		SWIG_SetErrorMsg(wtError, wiredtiger_strerror(result));
	}
	SWIG_fail;
} while(0)
%enddef

/* Error handling.  Default case: a non-zero return is an error. */
%exception {
	$action
	if (result != 0)
		SWIG_ERROR_IF_NOT_SET(result);
}

/* An API that returns a value that shouldn't be checked uses this. */
%define ANY_OK(m)
%exception m {
	$action
}
%enddef

/* Cursor positioning methods can also return WT_NOTFOUND. */
%define NOTFOUND_OK(m)
%exception m {
	$action
	if (result != 0 && result != WT_NOTFOUND)
		SWIG_ERROR_IF_NOT_SET(result);
}
%enddef

/* Cursor compare can return any of -1, 0, 1. */
%define COMPARE_OK(m)
%exception m {
	$action
	if (result < -1 || result > 1)
		SWIG_ERROR_IF_NOT_SET(result);
}
%enddef

/* Cursor compare can return any of -1, 0, 1 or WT_NOTFOUND. */
%define COMPARE_NOTFOUND_OK(m)
%exception m {
	$action
	if ((result < -1 || result > 1) && result != WT_NOTFOUND)
		SWIG_ERROR_IF_NOT_SET(result);
}
%enddef

NOTFOUND_OK(__wt_cursor::next)
NOTFOUND_OK(__wt_cursor::prev)
NOTFOUND_OK(__wt_cursor::remove)
NOTFOUND_OK(__wt_cursor::search)
NOTFOUND_OK(__wt_cursor::update)
NOTFOUND_OK(__wt_cursor::_modify)
NOTFOUND_OK(__wt_cursor::largest_key)
ANY_OK(__wt_modify::__wt_modify)
ANY_OK(__wt_modify::~__wt_modify)

COMPARE_OK(__wt_cursor::_compare)
COMPARE_OK(__wt_cursor::_equals)
COMPARE_NOTFOUND_OK(__wt_cursor::_search_near)

/* Lastly, some methods need no (additional) error checking. */
%exception __wt_connection::get_home;
%exception __wt_connection::is_new;
%exception __wt_connection::search_near;
%exception __wt_cursor::_set_key;
%exception __wt_cursor::_set_key_str;
%exception __wt_cursor::_set_value;
%exception __wt_cursor::_set_value_str;
%exception wiredtiger_strerror;
%exception wiredtiger_version;
%exception diagnostic_build;
%exception standalone_build;

/* WT_CURSOR customization. */
/* First, replace the varargs get / set methods with Python equivalents. */
%ignore __wt_cursor::get_key;
%ignore __wt_cursor::get_value;
%ignore __wt_cursor::set_key;
%ignore __wt_cursor::set_value;
%ignore __wt_cursor::modify(WT_CURSOR *, WT_MODIFY *, int);
%rename (modify) __wt_cursor::_modify;
%ignore __wt_modify::data;
%ignore __wt_modify::offset;
%ignore __wt_modify::size;

/* Next, override methods that return integers via arguments. */
%ignore __wt_cursor::compare(WT_CURSOR *, WT_CURSOR *, int *);
%ignore __wt_cursor::equals(WT_CURSOR *, WT_CURSOR *, int *);
%ignore __wt_cursor::search_near(WT_CURSOR *, int *);

OVERRIDE_METHOD(__wt_cursor, WT_CURSOR, compare, (self, other))
OVERRIDE_METHOD(__wt_cursor, WT_CURSOR, equals, (self, other))
OVERRIDE_METHOD(__wt_cursor, WT_CURSOR, search_near, (self))

/* SWIG magic to turn Python byte strings into data / size. */
%apply (char *STRING, int LENGTH) { (char *data, int size) };

/* Handle binary data returns from get_key/value -- avoid cstring.i: it creates a list of returns. */
%typemap(in,numinputs=0) (char **datap, int *sizep) (char *data, int size) { $1 = &data; $2 = &size; }
%typemap(in,numinputs=0) (char **charp, int *sizep) (char *data, int size) { $1 = &data; $2 = &size; }
%typemap(frearg) (char **datap, int *sizep) "";
%typemap(argout) (char **charp, int *sizep) {
	if (*$1)
		$result = PyUnicode_FromStringAndSize(*$1, *$2);
}

%typemap(argout) (char **datap, int *sizep) {
	if (*$1)
		$result = PyBytes_FromStringAndSize(*$1, *$2);
 }

/* Handle binary data input from FILE_HANDLE->fh_write. */
%typemap(in,numinputs=1) (size_t length, const void *buf) (Py_ssize_t length, const void *buf = NULL) {
    if (PyBytes_AsStringAndSize($input, &buf, &length) < 0)
        SWIG_exception_fail(SWIG_AttributeError,
          "bad bytes input argument");
    $1 = length;
    $2 = buf;
}

/* Handle binary data input from FILE_HANDLE->fh_read. */
%typemap(in,numinputs=1) (size_t length, void *buf) (Py_ssize_t length, const void *buf = NULL) {
      if (PyBytes_AsStringAndSize($input, &buf, &length) < 0)
          SWIG_exception_fail(SWIG_AttributeError,
            "bad bytes input argument");
    $1 = length;
    $2 = buf;
}

/* Handle record number returns from get_recno */
%typemap(in,numinputs=0) (uint64_t *recnop) (uint64_t recno) { $1 = &recno; }
%typemap(frearg) (uint64_t *recnop) "";
%typemap(argout) (uint64_t *recnop) { $result = PyLong_FromUnsignedLongLong(*$1); }

/* Handle returned hexadecimal timestamps. */
%typemap(in,numinputs=0) (char *hex_timestamp) (char tsbuf[WT_TS_HEX_STRING_SIZE]) { $1 = tsbuf; }
%typemap(argout) (char *hex_timestamp) {
	if (*$1)
		$result = SWIG_FromCharPtr($1);
}

%{
typedef int int_void;
%}
typedef int int_void;
%typemap(out) int_void { $result = VOID_Object; }

%extend __wt_cursor {
	/* Get / set keys and values */
	void _set_key(unsigned char *data, int size) {
		WT_ITEM k;
		k.data = data;
		k.size = (uint32_t)size;
		$self->set_key($self, &k);
	}

	/* Get / set keys and values */
	void _set_key_str(char *str) {
		$self->set_key($self, str);
	}

	int_void _set_recno(uint64_t recno) {
		WT_ITEM k;
		uint8_t recno_buf[20];
		size_t size;
		int ret;
		if ((ret = wiredtiger_struct_size($self->session,
		    &size, "r", recno)) != 0 ||
		    (ret = wiredtiger_struct_pack($self->session,
		    recno_buf, sizeof (recno_buf), "r", recno)) != 0)
			return (ret);

		k.data = recno_buf;
		k.size = (uint32_t)size;
		$self->set_key($self, &k);
		return (ret);
	}

	void _set_value(unsigned char *data, int size) {
		WT_ITEM v;
		v.data = data;
		v.size = (uint32_t)size;
		$self->set_value($self, &v);
	}

	/* Get / set keys and values */
	void _set_value_str(char *str) {
		$self->set_value($self, str);
	}

	/* Don't return values, just throw exceptions on failure. */
	int_void _get_key(char **datap, int *sizep) {
		WT_ITEM k;
		int ret = $self->get_key($self, &k);
		if (ret == 0) {
			*datap = (char *)k.data;
			*sizep = (int)k.size;
		}
		return (ret);
	}

	int_void _get_json_key(char **charp, int *sizep) {
		const char *k;
		int ret = $self->get_key($self, &k);
		if (ret == 0) {
			*charp = (char *)k;
			*sizep = strlen(k);
		}
		return (ret);
	}

	int_void _get_recno(uint64_t *recnop) {
		WT_ITEM k;
		int ret = $self->get_key($self, &k);
		if (ret == 0)
			ret = wiredtiger_struct_unpack($self->session,
			    k.data, k.size, "q", recnop);
		return (ret);
	}

	int_void _get_value(char **datap, int *sizep) {
		WT_ITEM v;
		int ret = $self->get_value($self, &v);
		if (ret == 0) {
			*datap = (char *)v.data;
			*sizep = (int)v.size;
		}
		return (ret);
	}

	int_void _get_json_value(char **charp, int *sizep) {
		const char *k;
		int ret = $self->get_value($self, &k);
		if (ret == 0) {
			*charp = (char *)k;
			*sizep = strlen(k);
		}
		return (ret);
	}

	/* compare: special handling. */
	int _compare(WT_CURSOR *other) {
		int cmp = 0;
		int ret = 0;
		if (other == NULL) {
			SWIG_Error(SWIG_NullReferenceError,
			    "in method 'Cursor_compare', "
			    "argument 1 of type 'struct __wt_cursor *' "
			    "is None");
			ret = EINVAL;  /* any non-zero value will do. */
		}
		else {
			ret = $self->compare($self, other, &cmp);

			/*
			 * Map less-than-zero to -1 and greater-than-zero to 1
			 * to avoid colliding with other errors.
			 */
			ret = (ret != 0) ? ret :
			    ((cmp < 0) ? -1 : (cmp == 0) ? 0 : 1);
		}
		return (ret);
	}

	/* equals: special handling. */
	int _equals(WT_CURSOR *other) {
		int cmp = 0;
		int ret = 0;
		if (other == NULL) {
			SWIG_Error(SWIG_NullReferenceError,
			    "in method 'Cursor_equals', "
			    "argument 1 of type 'struct __wt_cursor *' "
			    "is None");
			ret = EINVAL;  /* any non-zero value will do. */
		}
		else {
			ret = $self->equals($self, other, &cmp);
			if (ret == 0)
				ret = cmp;
		}
		return (ret);
	}

	/* search_near: special handling. */
	int _search_near() {
		int cmp = 0;
		int ret = $self->search_near($self, &cmp);
		/*
		 * Map less-than-zero to -1 and greater-than-zero to 1 to avoid
		 * colliding with other errors.
		 */
		return ((ret != 0) ? ret : (cmp < 0) ? -1 : (cmp == 0) ? 0 : 1);
	}

	int _freecb() {
		return (cursorFreeHandler($self));
	}

	/*
	 * modify: the size of the array was put into the first element by the
	 * typemap.
	 */
	int _modify(WT_MODIFY *list) {
		int count = (int)list[0].size;
		return (self->modify(self, &list[1], count));
	}

%pythoncode %{
	def get_key(self):
		'''get_key(self) -> object
		
		@copydoc WT_CURSOR::get_key
		Returns only the first column.'''
		k = self.get_keys()
		if len(k) == 1:
			return k[0]
		return k

	def get_keys(self):
		'''get_keys(self) -> (object, ...)
		
		@copydoc WT_CURSOR::get_key'''
		if self.is_json:
			return [self._get_json_key()]
		elif self.is_column:
			return [self._get_recno(),]
		else:
			return unpack(self.key_format, self._get_key())

	def get_value(self):
		'''get_value(self) -> object
		
		@copydoc WT_CURSOR::get_value
		Returns only the first column.'''
		v = self.get_values()
		if len(v) == 1:
			return v[0]
		return v

	def get_values(self):
		'''get_values(self) -> (object, ...)
		
		@copydoc WT_CURSOR::get_value'''
		if self.is_json:
			return [self._get_json_value()]
		else:
			return unpack(self.value_format, self._get_value())

	def set_key(self, *args):
		'''set_key(self) -> None
		
		@copydoc WT_CURSOR::set_key'''
		if len(args) == 1 and type(args[0]) == tuple:
			args = args[0]
		if self.is_column:
			self._set_recno(_wt_recno(args[0]))
		elif self.is_json:
			self._set_key_str(args[0])
		else:
			# Keep the Python string pinned
			self._key = pack(self.key_format, *args)
			self._set_key(self._key)

	def set_value(self, *args):
		'''set_value(self) -> None
		
		@copydoc WT_CURSOR::set_value'''
		if self.is_json:
			self._set_value_str(args[0])
		else:
			if len(args) == 1 and type(args[0]) == tuple:
				args = args[0]
			# Keep the Python string pinned
			self._value = pack(self.value_format, *args)
			self._set_value(self._value)

	def __iter__(self):
		'''Cursor objects support iteration, equivalent to calling
		WT_CURSOR::next until it returns ::WT_NOTFOUND.'''
		if not hasattr(self, '_iterable'):
			self._iterable = IterableCursor(self)
		return self._iterable

	def __delitem__(self, key):
		'''Python convenience for removing'''
		self.set_key(key)
		if self.remove() != 0:
			raise KeyError

	def __getitem__(self, key):
		'''Python convenience for searching'''
		self.set_key(key)
		if self.search() != 0:
			raise KeyError
		return self.get_value()

	def __setitem__(self, key, value):
		'''Python convenience for inserting'''
		self.set_key(key)
		self.set_value(value)
		if self.insert() != 0:
			raise KeyError
%}
};

/*
 * Support for WT_CURSOR.modify.  The WT_MODIFY object is known to
 * SWIG, but its attributes are regular Python attributes.
 * We extract the attributes at the call site to WT_CURSOR.modify
 * so we don't have to deal with managing Python objects references.
 */
%extend __wt_modify {
%pythoncode %{
	def __init__(self, data = '', offset = 0, size = 0):
		self.data = data
		self.offset = offset
		self.size = size

	def __repr__(self):
		return 'Modify(\'%s\', %d, %d)' % (self.data, self.offset, self.size)
%}
};

%extend __wt_session {
	int _log_printf(const char *msg) {
		return self->log_printf(self, "%s", msg);
	}

	int _freecb() {
		return (sessionFreeHandler(self));
	}
};

%extend __wt_connection {
	int _freecb() {
		return (0);
	}
};

%define CONCAT(a,b)   a##b
%enddef

 /*
  * SIDESTEP_METHOD is a workaround.  We don't yet have some methods exposed in
  * a way that makes them callable.  For some reason, this workaround works,
  * even though it's awkward.
  */
%define SIDESTEP_METHOD(cclass, method, cargs, cargs_call)
%ignore cclass::method;
%rename (method) cclass::CONCAT(_,method);
%extend cclass {
    int CONCAT(_, method) cargs {
        return (self->method cargs_call );
     }
};
%enddef

SIDESTEP_METHOD(__wt_storage_source, ss_customize_file_system,
  (WT_SESSION *session, const char *bucket_name,
    const char *auth_token, const char *config, WT_FILE_SYSTEM **file_systemp),
  (self, session, bucket_name, auth_token, config, file_systemp))

SIDESTEP_METHOD(__wt_storage_source, ss_flush,
  (WT_SESSION *session, WT_FILE_SYSTEM *file_system,
    const char *source, const char *object, const char *config),
  (self, session, file_system, source, object, config))

SIDESTEP_METHOD(__wt_storage_source, ss_flush_finish,
  (WT_SESSION *session, WT_FILE_SYSTEM *file_system,
    const char *source, const char *object, const char *config),
  (self, session, file_system, source, object, config))

SIDESTEP_METHOD(__wt_storage_source, terminate,
  (WT_SESSION *session),
  (self, session))

SIDESTEP_METHOD(__wt_file_system, fs_exist,
  (WT_SESSION *session, const char *name, bool *existp),
  (self, session, name, existp))

SIDESTEP_METHOD(__wt_file_system, fs_open_file,
  (WT_SESSION *session, const char *name, WT_FS_OPEN_FILE_TYPE file_type,
    uint32_t flags, WT_FILE_HANDLE **file_handlep),
  (self, session, name, file_type, flags, file_handlep))

SIDESTEP_METHOD(__wt_file_system, fs_remove,
  (WT_SESSION *session, const char *name, uint32_t flags),
  (self, session, name, flags))

SIDESTEP_METHOD(__wt_file_system, fs_rename,
  (WT_SESSION *session, const char *from, const char *to, uint32_t flags),
  (self, session, from, to, flags))

SIDESTEP_METHOD(__wt_file_system, fs_size,
  (WT_SESSION *session, const char *name, wt_off_t *sizep),
  (self, session, name, sizep))

SIDESTEP_METHOD(__wt_file_system, terminate,
  (WT_SESSION *session),
  (self, session))

SIDESTEP_METHOD(__wt_file_handle, close,
  (WT_SESSION *session),
  (self, session))

SIDESTEP_METHOD(__wt_file_handle, fh_advise,
  (WT_SESSION *session, wt_off_t offset, wt_off_t len, int advice),
  (self, session, offset, len, advice))

SIDESTEP_METHOD(__wt_file_handle, fh_extend,
  (WT_SESSION *session, wt_off_t offset),
  (self, session, offset))

SIDESTEP_METHOD(__wt_file_handle, fh_extend_nolock,
  (WT_SESSION *session, wt_off_t offset),
  (self, session, offset))

SIDESTEP_METHOD(__wt_file_handle, fh_lock,
  (WT_SESSION *session, bool lock),
  (self, session, lock))

SIDESTEP_METHOD(__wt_file_handle, fh_map,
  (WT_SESSION *session, bool lock, void *mapped_regionp, size_t *lengthp, void *mapped_cookiep),
  (self, session, mapped_regionp, lengthp, mapped_cookiep))

SIDESTEP_METHOD(__wt_file_handle, fh_map_discard,
  (WT_SESSION *session, void *map, size_t length, void *mapped_cookie),
  (self, session, map, length, mapped_cookie))

SIDESTEP_METHOD(__wt_file_handle, fh_map_preload,
  (WT_SESSION *session, const void *map, size_t length, void *mapped_cookie),
  (self, session, map, length, mapped_cookie))

SIDESTEP_METHOD(__wt_file_handle, fh_unmap,
  (WT_SESSION *session, void *mapped_region, size_t length, void *mapped_cookie),
  (self, session, mapped_region, length, mapped_cookie))

   /*
SIDESTEP_METHOD(__wt_file_handle, fh_read,
  (WT_SESSION *session, wt_off_t offset, size_t len, void *buf),
  (self, session, offset, len, buf))
   */

SIDESTEP_METHOD(__wt_file_handle, fh_size,
  (WT_SESSION *session, wt_off_t *sizep),
  (self, session, sizep))

SIDESTEP_METHOD(__wt_file_handle, fh_sync,
  (WT_SESSION *session),
  (self, session))

SIDESTEP_METHOD(__wt_file_handle, fh_sync_nowait,
  (WT_SESSION *session),
  (self, session))

SIDESTEP_METHOD(__wt_file_handle, fh_truncate,
  (WT_SESSION *session, wt_off_t offset),
  (self, session, offset))

SIDESTEP_METHOD(__wt_file_handle, fh_write,
  (WT_SESSION *session, unsigned long offset, size_t length, const void *buf),
  (self, session, offset, length, buf))

%ignore __wt_file_handle::fh_read;
%rename (fh_read) __wt_file_handle::_fh_read;
%extend __wt_file_handle {
    int _fh_read(WT_SESSION *session, unsigned long offset, size_t length, void *buf) {
        return (self->fh_read(self, session, offset, length, buf));
    }
};

%ignore __wt_file_system::fs_directory_list;
%ignore __wt_file_system::fs_directory_list_single;
%rename (fs_directory_list) __wt_file_system::_fs_directory_list;
%rename (fs_directory_list_single) __wt_file_system::_fs_directory_list_single;
%extend __wt_file_system {
    int _fs_directory_list(WT_SESSION *session, const char *directory, const char *prefix,
      char ***dirlist, int *countp) {
        return (self->fs_directory_list(self, session, directory, prefix, dirlist, countp));
    }
    int _fs_directory_list_single(WT_SESSION *session, const char *directory, const char *prefix,
      char ***dirlist, int *countp) {
        return (self->fs_directory_list_single(self, session, directory, prefix, dirlist, countp));
    }
};

/*
 * No need for a directory_list_free method, as the list and its components
 * are freed immediately after the directory_list call.
 */
%ignore __wt_file_system::fs_directory_list_free;

%{
int diagnostic_build() {
#ifdef HAVE_DIAGNOSTIC
	return 1;
#else
	return 0;
#endif
}
%}
int diagnostic_build();

%{
int standalone_build() {
#ifdef WT_STANDALONE_BUILD
	return 1;
#else
	return 0;
#endif
}
%}
int standalone_build();

/* Remove / rename parts of the C API that we don't want in Python. */
%immutable __wt_cursor::session;
%immutable __wt_cursor::uri;
%ignore __wt_cursor::key_format;
%ignore __wt_cursor::value_format;
%immutable __wt_session::connection;

%ignore __wt_collator;
%ignore __wt_compressor;
%ignore __wt_config_item;
%ignore __wt_data_source;
%ignore __wt_encryptor;
%ignore __wt_event_handler;
%ignore __wt_extractor;
%ignore __wt_item;
%ignore __wt_lsn;

%ignore __wt_connection::add_collator;
%ignore __wt_connection::add_compressor;
%ignore __wt_connection::add_data_source;
%ignore __wt_connection::add_encryptor;
%ignore __wt_connection::add_extractor;
%ignore __wt_connection::get_extension_api;
%ignore __wt_session::log_printf;

OVERRIDE_METHOD(__wt_session, WT_SESSION, log_printf, (self, msg))

%ignore wiredtiger_struct_pack;
%ignore wiredtiger_struct_size;
%ignore wiredtiger_struct_unpack;

%ignore wiredtiger_calc_modify;
%ignore wiredtiger_extension_init;
%ignore wiredtiger_extension_terminate;


/* Convert 'int *' to output args for wiredtiger_version */
%apply int *OUTPUT { int * };

%rename(Cursor) __wt_cursor;
%rename(Modify) __wt_modify;
%rename(Session) __wt_session;
%rename(Connection) __wt_connection;
%rename(FileHandle) __wt_file_handle;
%rename(StorageSource) __wt_storage_source;
%rename(FileSystem) __wt_file_system;

%include "wiredtiger.h"

/*
 * The original wiredtiger_calc_modify was ignored, now we define our own.
 * Python needs to know whether to return a bytes object or a string.
 * Part of the smarts to do that is the output typemap, which matches on
 * the naming of the parameter: entries vs. entries_string
 */
extern int _wiredtiger_calc_modify(WT_SESSION *session,
    const WT_ITEM *oldv, const WT_ITEM *newv,
    size_t maxdiff, WT_MODIFY *entries, int *nentriesp);
extern int _wiredtiger_calc_modify_string(WT_SESSION *session,
    const WT_ITEM *oldv, const WT_ITEM *newv,
    size_t maxdiff, WT_MODIFY *entries_string, int *nentriesp);
%{
int _wiredtiger_calc_modify(WT_SESSION *session,
    const WT_ITEM *oldv, const WT_ITEM *newv,
    size_t maxdiff, WT_MODIFY *entries, int *nentriesp)
{
	return (wiredtiger_calc_modify(
	    session, oldv, newv, maxdiff, entries, nentriesp));
}

int _wiredtiger_calc_modify_string(WT_SESSION *session,
    const WT_ITEM *oldv, const WT_ITEM *newv,
    size_t maxdiff, WT_MODIFY *entries_string, int *nentriesp)
{
	return (wiredtiger_calc_modify(
	    session, oldv, newv, maxdiff, entries_string, nentriesp));
}

/* Add event handler support. */

static void
freeModifyArray(WT_MODIFY *modarray)
{
	size_t i, len;

	len = modarray[0].size;
	for (i = 1; i <= len; i++)
		__wt_free(NULL, modarray[i].data.data);
	__wt_free(NULL, modarray);
}

static int unpackBytesOrString(PyObject *obj, void **datap, size_t *sizep)
{
	void *data;
	Py_ssize_t sz;

	if (PyBytes_AsStringAndSize(obj, &data, &sz) < 0) {
#if PY_VERSION_HEX >= 0x03000000
		PyErr_Clear();
		if ((data = PyUnicode_AsUTF8AndSize(obj, &sz)) != 0)
			*sizep = strlen((char *)data) + 1;
		else
#endif
			return (-1);
	}
	*datap = data;
	*sizep = sz;
	return (0);
}

/* Write to and flush the stream. */
static int
writeToPythonStream(const char *streamname, const char *message)
{
	PyObject *sys, *se, *write_method, *flush_method, *written,
	    *arglist, *arglist2;
	char *msg;
	int ret;
	size_t msglen;

	sys = NULL;
	se = NULL;
	write_method = flush_method = NULL;
	written = NULL;
	arglist = arglist2 = NULL;
	msglen = strlen(message);
	WT_RET(__wt_malloc(NULL, msglen + 2, &msg));
	strcpy(msg, message);
	strcpy(&msg[msglen], "\n");

	/* Acquire python Global Interpreter Lock. Otherwise can segfault. */
	SWIG_PYTHON_THREAD_BEGIN_BLOCK; 

	ret = 1;
	if ((sys = PyImport_ImportModule("sys")) == NULL)
		goto err;
	if ((se = PyObject_GetAttrString(sys, streamname)) == NULL)
		goto err;
	if ((write_method = PyObject_GetAttrString(se, "write")) == NULL)
		goto err;
	if ((flush_method = PyObject_GetAttrString(se, "flush")) == NULL)
		goto err;
	if ((arglist = Py_BuildValue("(s)", msg)) == NULL)
		goto err;
	if ((arglist2 = Py_BuildValue("()")) == NULL)
		goto err;

	written = PyObject_CallObject(write_method, arglist);
	(void)PyObject_CallObject(flush_method, arglist2);
	ret = 0;

err:	Py_XDECREF(arglist2);
	Py_XDECREF(arglist);
	Py_XDECREF(flush_method);
	Py_XDECREF(write_method);
	Py_XDECREF(se);
	Py_XDECREF(sys);
	Py_XDECREF(written);

	/* Release python Global Interpreter Lock */
	SWIG_PYTHON_THREAD_END_BLOCK;

	__wt_free(NULL, msg);
	return (ret);
}

static int
pythonErrorCallback(WT_EVENT_HANDLER *handler, WT_SESSION *session, int err,
    const char *message)
{
	return writeToPythonStream("stderr", message);
}

static int
pythonMessageCallback(WT_EVENT_HANDLER *handler, WT_SESSION *session,
    const char *message)
{
	return writeToPythonStream("stdout", message);
}

/* Zero out SWIG's pointer to the C object,
 * equivalent to 'pyobj.this = None' in Python.
 */
static int
pythonClose(PY_CALLBACK *pcb)
{
	int ret;

	/*
	 * Ensure the global interpreter lock is held - so that Python
	 * doesn't shut down threads while we use them.
	 */
	SWIG_PYTHON_THREAD_BEGIN_BLOCK;

	ret = 0;
	if (PyObject_SetAttrString(pcb->pyobj, "this", Py_None) == -1) {
		SWIG_Error(SWIG_RuntimeError, "WT SetAttr failed");
		ret = EINVAL;  /* any non-zero value will do. */
	}
	Py_XDECREF(pcb->pyobj);

	SWIG_PYTHON_THREAD_END_BLOCK;

	return (ret);
}

/* Session specific close handler. */
static int
sessionCloseHandler(WT_SESSION *session_arg)
{
	int ret;
	PY_CALLBACK *pcb;
	WT_SESSION_IMPL *session;

	ret = 0;
	session = (WT_SESSION_IMPL *)session_arg;
	pcb = (PY_CALLBACK *)session->lang_private;
	session->lang_private = NULL;
	if (pcb != NULL)
		ret = pythonClose(pcb);
	__wt_free(session, pcb);

	return (ret);
}

/* Cursor specific close handler. */
static int
cursorCloseHandler(WT_CURSOR *cursor)
{
	int ret;
	PY_CALLBACK *pcb;

	ret = 0;
	pcb = (PY_CALLBACK *)cursor->lang_private;
	cursor->lang_private = NULL;
	if (pcb != NULL)
		ret = pythonClose(pcb);
	__wt_free(CUR2S(cursor), pcb);

	return (ret);
}

/* Session specific close handler. */
static int
sessionFreeHandler(WT_SESSION *session_arg)
{
	PY_CALLBACK *pcb;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)session_arg;
	pcb = (PY_CALLBACK *)session->lang_private;
	session->lang_private = NULL;
	__wt_free(session, pcb);
	return (0);
}

/* Cursor specific close handler. */
static int
cursorFreeHandler(WT_CURSOR *cursor)
{
	PY_CALLBACK *pcb;

	pcb = (PY_CALLBACK *)cursor->lang_private;
	cursor->lang_private = NULL;
	__wt_free(CUR2S(cursor), pcb);
	return (0);
}

static int
pythonCloseCallback(WT_EVENT_HANDLER *handler, WT_SESSION *session,
    WT_CURSOR *cursor)
{
	int ret;

	WT_UNUSED(handler);

	if (cursor != NULL)
		ret = cursorCloseHandler(cursor);
	else
		ret = sessionCloseHandler(session);
	return (ret);
}

static WT_EVENT_HANDLER pyApiEventHandler = {
	pythonErrorCallback, pythonMessageCallback, NULL, pythonCloseCallback
};
%}

%pythoncode %{
class stat:
	'''keys for statistics cursors'''

	class conn:
		'''keys for cursors on connection statistics'''
		pass

	class dsrc:
		'''keys for cursors on data source statistics'''
		pass

	class session:
		'''keys for cursors on session statistics'''
		pass

## @}

import sys
# All names starting with 'WT_STAT_DSRC_' are renamed to
# the wiredtiger.stat.dsrc class, those starting with 'WT_STAT_CONN' are
# renamed to the wiredtiger.stat.conn class. All names starting with 'WT_STAT_SESSION'
# are renamed to the wiredtiger.stat.session class.
def _rename_with_prefix(prefix, toclass):
	curmodule = sys.modules[__name__]
	for name in dir(curmodule):
		if name.startswith(prefix):
			shortname = name[len(prefix):].lower()
			setattr(toclass, shortname, getattr(curmodule, name))
			delattr(curmodule, name)

_rename_with_prefix('WT_STAT_CONN_', stat.conn)
_rename_with_prefix('WT_STAT_DSRC_', stat.dsrc)
_rename_with_prefix('WT_STAT_SESSION_', stat.session)
_rename_with_prefix('WT_FS_', FileSystem)
_rename_with_prefix('WT_FILE_HANDLE_', FileHandle)
del _rename_with_prefix
%}
