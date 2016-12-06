/*-
 * Public Domain 2014-2016 MongoDB, Inc.
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
%define DOCSTRING
"Python wrappers around the WiredTiger C API

This provides an API similar to the C API, with the following modifications:
  - Many C functions are exposed as OO methods. See the Python examples and test suite
  - Errors are handled in a Pythonic way; wrap calls in try/except blocks
  - Cursors have extra accessor methods and iterators that are higher-level than the C API
  - Statistics cursors behave a little differently and are best handled using the C-like functions
  - C Constants starting with WT_STAT_DSRC are instead exposed under wiredtiger.stat.dsrc
  - C Constants starting with WT_STAT_CONN are instead exposed under wiredtiger.stat.conn
"
%enddef

%module(docstring=DOCSTRING) wiredtiger

%feature("autodoc", "0");

%pythoncode %{
from packing import pack, unpack
## @endcond
%}

/* Set the input argument to point to a temporary variable */ 
%typemap(in, numinputs=0) WT_CONNECTION ** (WT_CONNECTION *temp = NULL) {
	$1 = &temp;
}
%typemap(in, numinputs=0) WT_SESSION ** (WT_SESSION *temp = NULL) {
	$1 = &temp;
}
%typemap(in, numinputs=0) WT_ASYNC_OP ** (WT_ASYNC_OP *temp = NULL) {
	$1 = &temp;
}
%typemap(in, numinputs=0) WT_CURSOR ** (WT_CURSOR *temp = NULL) {
	$1 = &temp;
}

%typemap(in) WT_ASYNC_CALLBACK * (PyObject *callback_obj = NULL) %{
	callback_obj = $input;
	$1 = &pyApiAsyncCallback;
%}

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
%typemap(argout) WT_ASYNC_OP ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
	    SWIGTYPE_p___wt_async_op, 0);
	if (*$1 != NULL) {
		PY_CALLBACK *pcb;

		(*$1)->c.flags |= WT_CURSTD_RAW;
		PyObject_SetAttrString($result, "is_column",
		    PyBool_FromLong(strcmp((*$1)->key_format, "r") == 0));
		PyObject_SetAttrString($result, "key_format",
		    PyString_InternFromString((*$1)->key_format));
		PyObject_SetAttrString($result, "value_format",
		    PyString_InternFromString((*$1)->value_format));

		if (__wt_calloc_def((WT_ASYNC_OP_IMPL *)(*$1), 1, &pcb) != 0)
			SWIG_exception_fail(SWIG_MemoryError, "WT calloc failed");
		else {
			pcb->pyobj = $result;
			Py_XINCREF(pcb->pyobj);
			/* XXX Is there a way to avoid SWIG's numbering? */
			pcb->pyasynccb = callback_obj5;
			Py_XINCREF(pcb->pyasynccb);
			(*$1)->c.lang_private = pcb;
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

/* 64 bit typemaps. */
%typemap(in) uint64_t {
	$1 = PyLong_AsUnsignedLongLong($input);
}
%typemap(out) uint64_t {
	$result = PyLong_FromUnsignedLongLong($1);
}

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
DESTRUCTOR(__wt_session, close)

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
	PyObject *pyobj;	/* the python Session/Cursor/AsyncOp object */
	PyObject *pyasynccb;	/* the callback to use for AsyncOp */
} PY_CALLBACK;

static PyObject *wtError;

static int sessionFreeHandler(WT_SESSION *session_arg);
static int cursorFreeHandler(WT_CURSOR *cursor_arg);
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

## @cond DISABLE
# Implements the iterable contract
class IterableCursor:
	def __init__(self, cursor):
		self.cursor = cursor

	def __iter__(self):
		return self

	def next(self):
		if self.cursor.next() == WT_NOTFOUND:
			raise StopIteration
		return self.cursor.get_keys() + self.cursor.get_values()
## @endcond

# An abstract class, which must be subclassed with notify() overridden.
class AsyncCallback:
	def __init__(self):
		raise NotImplementedError

	def notify(self, op, op_ret, flags):
		raise NotImplementedError

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
SELFHELPER(struct __wt_async_op, op)
SELFHELPER(struct __wt_session, session)
SELFHELPER(struct __wt_cursor, cursor)

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

/* Async operations can return EBUSY when no ops are available. */
%define EBUSY_OK(m)
%exception m {
retry:
	$action
	if (result != 0 && result != EBUSY)
		SWIG_ERROR_IF_NOT_SET(result);
	else if (result == EBUSY) {
		SWIG_PYTHON_THREAD_BEGIN_ALLOW;
		__wt_sleep(0, 10000);
		SWIG_PYTHON_THREAD_END_ALLOW;
		goto retry;
	}
}
%enddef

/* Any API that returns an enum type uses this. */
%define ENUM_OK(m)
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

EBUSY_OK(__wt_connection::async_new_op)
ENUM_OK(__wt_async_op::get_type)
NOTFOUND_OK(__wt_cursor::next)
NOTFOUND_OK(__wt_cursor::prev)
NOTFOUND_OK(__wt_cursor::remove)
NOTFOUND_OK(__wt_cursor::search)
NOTFOUND_OK(__wt_cursor::update)

COMPARE_OK(__wt_cursor::_compare)
COMPARE_OK(__wt_cursor::_equals)
COMPARE_NOTFOUND_OK(__wt_cursor::_search_near)

/* Lastly, some methods need no (additional) error checking. */
%exception __wt_connection::get_home;
%exception __wt_connection::is_new;
%exception __wt_connection::search_near;
%exception __wt_async_op::_set_key;
%exception __wt_async_op::_set_value;
%exception __wt_cursor::_set_key;
%exception __wt_cursor::_set_key_str;
%exception __wt_cursor::_set_value;
%exception __wt_cursor::_set_value_str;
%exception wiredtiger_strerror;
%exception wiredtiger_version;
%exception diagnostic_build;
%exception verbose_build;

/* WT_ASYNC_OP customization. */
/* First, replace the varargs get / set methods with Python equivalents. */
%ignore __wt_async_op::get_key;
%ignore __wt_async_op::get_value;
%ignore __wt_async_op::set_key;
%ignore __wt_async_op::set_value;
%immutable __wt_async_op::connection;

/* WT_CURSOR customization. */
/* First, replace the varargs get / set methods with Python equivalents. */
%ignore __wt_cursor::get_key;
%ignore __wt_cursor::get_value;
%ignore __wt_cursor::set_key;
%ignore __wt_cursor::set_value;

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
%typemap(frearg) (char **datap, int *sizep) "";
%typemap(argout) (char **datap, int *sizep) {
	if (*$1)
		$result = SWIG_FromCharPtrAndSize(*$1, *$2);
}

/* Handle record number returns from get_recno */
%typemap(in,numinputs=0) (uint64_t *recnop) (uint64_t recno) { $1 = &recno; }
%typemap(frearg) (uint64_t *recnop) "";
%typemap(argout) (uint64_t *recnop) { $result = PyLong_FromUnsignedLongLong(*$1); }

%{
typedef int int_void;
%}
typedef int int_void;
%typemap(out) int_void { $result = VOID_Object; }

%extend __wt_async_op {
	/* Get / set keys and values */
	void _set_key(char *data, int size) {
		WT_ITEM k;
		k.data = data;
		k.size = (uint32_t)size;
		$self->set_key($self, &k);
	}

	int_void _set_recno(uint64_t recno) {
		WT_ITEM k;
		uint8_t recno_buf[20];
		size_t size;
		int ret;
		if ((ret = wiredtiger_struct_size(NULL,
		    &size, "r", recno)) != 0 ||
		    (ret = wiredtiger_struct_pack(NULL,
		    recno_buf, sizeof (recno_buf), "r", recno)) != 0)
			return (ret);

		k.data = recno_buf;
		k.size = (uint32_t)size;
		$self->set_key($self, &k);
		return (ret);
	}

	void _set_value(char *data, int size) {
		WT_ITEM v;
		v.data = data;
		v.size = (uint32_t)size;
		$self->set_value($self, &v);
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

	int_void _get_recno(uint64_t *recnop) {
		WT_ITEM k;
		int ret = $self->get_key($self, &k);
		if (ret == 0)
			ret = wiredtiger_struct_unpack(NULL,
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

	int _freecb() {
		return (cursorFreeHandler($self));
	}

%pythoncode %{
	def get_key(self):
		'''get_key(self) -> object
		
		@copydoc WT_ASYNC_OP::get_key
		Returns only the first column.'''
		k = self.get_keys()
		if len(k) == 1:
			return k[0]
		return k

	def get_keys(self):
		'''get_keys(self) -> (object, ...)
		
		@copydoc WT_ASYNC_OP::get_key'''
		if self.is_column:
			return [self._get_recno(),]
		else:
			return unpack(self.key_format, self._get_key())

	def get_value(self):
		'''get_value(self) -> object
		
		@copydoc WT_ASYNC_OP::get_value
		Returns only the first column.'''
		v = self.get_values()
		if len(v) == 1:
			return v[0]
		return v

	def get_values(self):
		'''get_values(self) -> (object, ...)
		
		@copydoc WT_ASYNC_OP::get_value'''
		return unpack(self.value_format, self._get_value())

	def set_key(self, *args):
		'''set_key(self) -> None
		
		@copydoc WT_ASYNC_OP::set_key'''
		if len(args) == 1 and type(args[0]) == tuple:
			args = args[0]
		if self.is_column:
			self._set_recno(long(args[0]))
		else:
			# Keep the Python string pinned
			self._key = pack(self.key_format, *args)
			self._set_key(self._key)

	def set_value(self, *args):
		'''set_value(self) -> None
		
		@copydoc WT_ASYNC_OP::set_value'''
		if len(args) == 1 and type(args[0]) == tuple:
			args = args[0]
		# Keep the Python string pinned
		self._value = pack(self.value_format, *args)
		self._set_value(self._value)

	def __getitem__(self, key):
		'''Python convenience for searching'''
		self.set_key(key)
		if self.search() != 0:
			raise KeyError
		return self.get_value()

	def __setitem__(self, key, value):
		'''Python convenience for inserting'''
		self.set_key(key)
		self.set_key(value)
		self.insert()
%}
};

%extend __wt_cursor {
	/* Get / set keys and values */
	void _set_key(char *data, int size) {
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

	void _set_value(char *data, int size) {
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

	int_void _get_json_key(char **datap, int *sizep) {
		const char *k;
		int ret = $self->get_key($self, &k);
		if (ret == 0) {
			*datap = (char *)k;
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

	int_void _get_json_value(char **datap, int *sizep) {
		const char *k;
		int ret = $self->get_value($self, &k);
		if (ret == 0) {
			*datap = (char *)k;
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
			self._set_recno(long(args[0]))
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

%{
int diagnostic_build() {
#ifdef HAVE_DIAGNOSTIC
	return 1;
#else
	return 0;
#endif
}

int verbose_build() {
#ifdef HAVE_VERBOSE
	return 1;
#else
	return 0;
#endif
}
%}
int diagnostic_build();
int verbose_build();

/* Remove / rename parts of the C API that we don't want in Python. */
%immutable __wt_cursor::session;
%immutable __wt_cursor::uri;
%ignore __wt_cursor::key_format;
%ignore __wt_cursor::value_format;
%immutable __wt_session::connection;
%immutable __wt_async_op::connection;
%immutable __wt_async_op::uri;
%immutable __wt_async_op::config;
%ignore __wt_async_op::key_format;
%ignore __wt_async_op::value_format;

%ignore __wt_async_callback;
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

%ignore wiredtiger_extension_init;
%ignore wiredtiger_extension_terminate;

/* Convert 'int *' to output args for wiredtiger_version */
%apply int *OUTPUT { int * };

%rename(AsyncOp) __wt_async_op;
%rename(Cursor) __wt_cursor;
%rename(Session) __wt_session;
%rename(Connection) __wt_connection;

%include "wiredtiger.h"

/* Add event handler support. */
%{
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
	msg = malloc(msglen + 2);
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

	if (msg)
		free(msg);
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
	Py_XDECREF(pcb->pyasynccb);

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
	__wt_free((WT_SESSION_IMPL *)cursor->session, pcb);

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
	__wt_free((WT_SESSION_IMPL *)cursor->session, pcb);
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

/* Add async callback support. */
%{

static int
pythonAsyncCallback(WT_ASYNC_CALLBACK *cb, WT_ASYNC_OP *asyncop, int opret,
    uint32_t flags)
{
	int ret, t_ret;
	PY_CALLBACK *pcb;
	PyObject *arglist, *notify_method, *pyresult;
	WT_ASYNC_OP_IMPL *op;
	WT_SESSION_IMPL *session;

	/*
	 * Ensure the global interpreter lock is held since we'll be
	 * making Python calls now.
	 */
	SWIG_PYTHON_THREAD_BEGIN_BLOCK;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	session = O2S(op);
	pcb = (PY_CALLBACK *)asyncop->c.lang_private;
	asyncop->c.lang_private = NULL;
	ret = 0;

	if (pcb->pyasynccb == NULL)
		goto err;
	if ((arglist = Py_BuildValue("(Oii)", pcb->pyobj,
	    opret, flags)) == NULL)
		goto err;
	if ((notify_method = PyObject_GetAttrString(pcb->pyasynccb,
	    "notify")) == NULL)
		goto err;

	pyresult = PyEval_CallObject(notify_method, arglist);
	if (pyresult == NULL || !PyArg_Parse(pyresult, "i", &ret))
		goto err;

	if (0) {
		if (ret == 0)
			ret = EINVAL;
err:		__wt_err(session, ret, "python async callback error");
	}
	Py_XDECREF(pyresult);
	Py_XDECREF(notify_method);
	Py_XDECREF(arglist);

	SWIG_PYTHON_THREAD_END_BLOCK;

	if (pcb != NULL) {
		if ((t_ret = pythonClose(pcb) != 0) && ret == 0)
			ret = t_ret;
	}
	__wt_free(session, pcb);

	if (ret == 0 && (opret == 0 || opret == WT_NOTFOUND))
		return (0);
	else
		return (1);
}

static WT_ASYNC_CALLBACK pyApiAsyncCallback = { pythonAsyncCallback };
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

## @}

import sys
# All names starting with 'WT_STAT_DSRC_' are renamed to
# the wiredtiger.stat.dsrc class, those starting with 'WT_STAT_CONN' are
# renamed to wiredtiger.stat.conn class.
def _rename_with_prefix(prefix, toclass):
	curmodule = sys.modules[__name__]
	for name in dir(curmodule):
		if name.startswith(prefix):
			shortname = name[len(prefix):].lower()
			setattr(toclass, shortname, getattr(curmodule, name))
			delattr(curmodule, name)

_rename_with_prefix('WT_STAT_CONN_', stat.conn)
_rename_with_prefix('WT_STAT_DSRC_', stat.dsrc)
del _rename_with_prefix
%}

