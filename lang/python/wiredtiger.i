/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 *
 * wiredtiger.i
 *	The SWIG interface file defining the wiredtiger python API.
 */

%define DOCSTRING
"@defgroup wt_python WiredTiger Python API
Python wrappers aroung the WiredTiger C API.
@{
@cond IGNORE"
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
%typemap(in, numinputs=0) WT_CURSOR ** (WT_CURSOR *temp = NULL) {
	$1 = &temp;
}

/* Event handlers are not supported in Python. */
%typemap(in, numinputs=0) WT_EVENT_HANDLER * { $1 = NULL; }

/* Set the return value to the returned connection, session, or cursor */
%typemap(argout) WT_CONNECTION ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
	    SWIGTYPE_p___wt_connection, 0);
}
%typemap(argout) WT_SESSION ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
	    SWIGTYPE_p___wt_session, 0);
}

%typemap(argout) WT_CURSOR ** {
	$result = SWIG_NewPointerObj(SWIG_as_voidptr(*$1),
	    SWIGTYPE_p___wt_cursor, 0);
	if (*$1 != NULL) {
		(*$1)->flags |= WT_CURSTD_RAW;
		PyObject_SetAttrString($result, "is_column",
		    PyBool_FromLong(strcmp((*$1)->key_format, "r") == 0));
		PyObject_SetAttrString($result, "key_format",
		    PyString_InternFromString((*$1)->key_format));
		PyObject_SetAttrString($result, "value_format",
		    PyString_InternFromString((*$1)->value_format));
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
		'''close(self, config) -> int
		
		@copydoc class::method'''
		try:
			return $action(self, *args)
		finally:
			self.this = None
%}
%enddef
DESTRUCTOR(__wt_connection, close)
DESTRUCTOR(__wt_cursor, close)
DESTRUCTOR(__wt_session, close)

/* Don't require empty config strings. */
%typemap(default) const char *config { $1 = NULL; }
%typemap(default) WT_CURSOR *to_dup { $1 = NULL; }

/* 
 * Error returns other than WT_NOTFOUND generate an exception.
 * Use our own exception type, in future tailored to the kind
 * of error.
 */
%header %{
static PyObject *wtError;
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
%}

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
	res = SWIG_ConvertPtr($input, &argp, $descriptor, $disown | 0);
	if (!SWIG_IsOK(res)) { 
		SWIG_exception_fail(SWIG_ArgError(res), "in method '$symname', "
		    "argument $argnum of type '$type'");
	}
	$2 = $1 = ($ltype)(argp);
%}
%enddef

SELFHELPER(struct __wt_connection, connection)
SELFHELPER(struct __wt_session, session)
SELFHELPER(struct __wt_cursor, cursor)

/* Error handling.  Default case: a non-zero return is an error. */
%exception {
	$action
	if (result != 0) {
		/* We could use PyErr_SetObject for more complex reporting. */
		SWIG_SetErrorMsg(wtError, wiredtiger_strerror(result));
		SWIG_fail;
	}
}

/* Cursor positioning methods can also return WT_NOTFOUND. */
%define NOTFOUND_OK(m)
%exception m {
	$action
	if (result != 0 && result != WT_NOTFOUND) {
		/* We could use PyErr_SetObject for more complex reporting. */
		SWIG_SetErrorMsg(wtError, wiredtiger_strerror(result));
		SWIG_fail;
	}
}
%enddef

/* Cursor compare can return any of -1, 0, 1 or WT_NOTFOUND. */
%define COMPARE_OK(m)
%exception m {
	$action
	if ((result < -1 || result > 1) && result != WT_NOTFOUND) {
		/* We could use PyErr_SetObject for more complex reporting. */
		SWIG_SetErrorMsg(wtError, wiredtiger_strerror(result));
		SWIG_fail;
	}
}
%enddef

NOTFOUND_OK(__wt_cursor::next)
NOTFOUND_OK(__wt_cursor::prev)
NOTFOUND_OK(__wt_cursor::remove)
NOTFOUND_OK(__wt_cursor::search)
NOTFOUND_OK(__wt_cursor::update)

COMPARE_OK(__wt_cursor::compare)
COMPARE_OK(__wt_cursor::search_near)

/* Lastly, some methods need no (additional) error checking. */
%exception __wt_connection::search_near;
%exception __wt_connection::get_home;
%exception __wt_connection::is_new;
%exception __wt_cursor::_set_key;
%exception __wt_cursor::_set_value;
%exception wiredtiger_strerror;
%exception wiredtiger_version;

/* WT_CURSOR customization. */
/* First, replace the varargs get / set methods with Python equivalents. */
%ignore __wt_cursor::get_key;
%ignore __wt_cursor::get_value;
%ignore __wt_cursor::set_key;
%ignore __wt_cursor::set_value;

/* Next, override methods that return integers via arguments. */
%ignore __wt_cursor::compare(WT_CURSOR *, WT_CURSOR *, int *);
%ignore __wt_cursor::search_near(WT_CURSOR *, int *);

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

%extend __wt_cursor {
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

	/* compare and search_near need special handling. */
	int compare(WT_CURSOR *other) {
		int cmp = 0;
		int ret = $self->compare($self, other, &cmp);
		/*
		 * Map less-than-zero to -1 and greater-than-zero to 1 to avoid
		 * colliding with other errors.
		 */
		return ((ret != 0) ? ret :
		    (cmp < 0) ? -1 : (cmp == 0) ? 0 : 1);
	}

	int search_near() {
		int cmp = 0;
		int ret = $self->search_near($self, &cmp);
		/*
		 * Map less-than-zero to -1 and greater-than-zero to 1 to avoid
		 * colliding with WT_NOTFOUND.
		 */
		return ((ret != 0) ? ret :
		    (cmp < 0) ? -1 : (cmp == 0) ? 0 : 1);
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
		if self.is_column:
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
		return unpack(self.value_format, self._get_value())

	def set_key(self, *args):
		'''set_key(self) -> None
		
		@copydoc WT_CURSOR::set_key'''
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
		
		@copydoc WT_CURSOR::set_value'''
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

	def __getitem__(self, key):
		'''Python convenience for searching'''
		self.set_key(key)
		if self.search() != 0:
			raise KeyError
		return self.get_value()
%}
};

/* Remove / rename parts of the C API that we don't want in Python. */
%immutable __wt_cursor::session;
%immutable __wt_cursor::uri;
%ignore __wt_cursor::key_format;
%ignore __wt_cursor::value_format;
%immutable __wt_session::connection;

%ignore __wt_buf;
%ignore __wt_config_item;
%ignore __wt_event_handler;
%ignore __wt_extractor;
%ignore __wt_item;

%ignore __wt_collator;
%ignore __wt_connection::add_collator;
%ignore __wt_compressor;
%ignore __wt_connection::add_compressor;
%ignore __wt_data_source;
%ignore __wt_connection::add_data_source;
%ignore __wt_connection::add_extractor;
%ignore __wt_connection::get_extension_api;

%ignore wiredtiger_struct_pack;
%ignore wiredtiger_struct_packv;
%ignore wiredtiger_struct_size;
%ignore wiredtiger_struct_sizev;
%ignore wiredtiger_struct_unpack;
%ignore wiredtiger_struct_unpackv;

%ignore wiredtiger_extension_init;
%ignore wiredtiger_extension_terminate;

/* Convert 'int *' to output args for wiredtiger_version */
%apply int *OUTPUT { int * };

%rename(Cursor) __wt_cursor;
%rename(Session) __wt_session;
%rename(Connection) __wt_connection;

%include "wiredtiger.h"

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

