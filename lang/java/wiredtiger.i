/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 *
 * wiredtiger.i
 *	The SWIG interface file defining the wiredtiger Java API.
 */

%module wiredtiger

%include "enums.swg"
%include "typemaps.i"

%pragma(java) jniclasscode=%{
  static {
    try {
	System.loadLibrary("wiredtiger_java");
    } catch (UnsatisfiedLinkError e) {
      System.err.println("Native code library failed to load. \n" + e);
      System.exit(1);
    }
  }
%}

%{
static void throwDbException(JNIEnv *jenv, const char *msg) {
	jclass excep = (*jenv)->FindClass(jenv, "com/wiredtiger/db/DbException");
	if (excep)
		(*jenv)->ThrowNew(jenv, excep, msg);
}
%}

/* No finalizers */
%typemap(javafinalize) SWIGTYPE ""

/* Event handlers are not supported in Java. */
%typemap(in, numinputs=0) WT_EVENT_HANDLER * %{ $1 = NULL; %}

/* Allow silently passing the Java object and JNIEnv into our code. */
%typemap(in, numinputs=0) jobject *jthis %{ $1 = jarg1_; %}
%typemap(in, numinputs=0) JNIEnv * %{ $1 = jenv; %}

/* 64 bit typemaps. */
%typemap(jni) uint64_t "jlong"
%typemap(jtype) uint64_t "long"
%typemap(jstype) uint64_t "long"

%typemap(javain) uint64_t "$javainput"
%typemap(javaout) uint64_t {
	return $jnicall;
}

/* Return byte[] from cursor.get_value */
%typemap(jni) WT_ITEM * "jbyteArray"
%typemap(jtype) WT_ITEM * "byte[]"
%typemap(jstype) WT_ITEM * "byte[]"

%typemap(javain) WT_ITEM * "$javainput"
%typemap(javaout) WT_ITEM * {
	return $jnicall;
}

%typemap(in) WT_ITEM * (WT_ITEM item) %{
	$1 = &item;
	$1->data = (*jenv)->GetByteArrayElements(jenv, $input, 0);
	$1->size = (*jenv)->GetArrayLength(jenv, $input);
%}

%typemap(argout) WT_ITEM * %{
	(*jenv)->ReleaseByteArrayElements(jenv, $input, $1->data, 0);
%}

%typemap(out) WT_ITEM * %{
	if ($1 == NULL)
		$result = NULL;
	else if (($result = (*jenv)->NewByteArray(jenv, $1->size)) != NULL) {
		(*jenv)->SetByteArrayRegion(jenv,
		    $result, 0, $1->size, $1->data);
	}
%}

/* Don't require empty config strings. */
%typemap(default) const char *config %{ $1 = NULL; %}

%typemap(out) int %{
	if ($1 != 0 && $1 != WT_NOTFOUND) {
		throwDbException(jenv, wiredtiger_strerror($1));
		return $null;
	}
	$result = $1;
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
%typemap(in, numinputs=0) type *name "$1 = *(type **)&jarg1;"
%enddef

SELFHELPER(struct __wt_connection, connection)
SELFHELPER(struct __wt_session, session)
SELFHELPER(struct __wt_cursor, cursor)

/* WT_CURSOR customization. */
/* First, replace the varargs get / set methods with Java equivalents. */
%ignore __wt_cursor::get_key;
%ignore __wt_cursor::get_value;
%ignore __wt_cursor::set_key;
%ignore __wt_cursor::set_value;
%ignore __wt_cursor::insert;
%ignore __wt_cursor::remove;
%ignore __wt_cursor::search;
%ignore __wt_cursor::search_near;
%ignore __wt_cursor::update;
%javamethodmodifiers __wt_cursor::next "protected";
%rename (next_wrap) __wt_cursor::next;
%javamethodmodifiers __wt_cursor::prev "protected";
%rename (prev_wrap) __wt_cursor::prev;

/* SWIG magic to turn Java byte strings into data / size. */
%apply (char *STRING, int LENGTH) { (char *data, int size) };

/* Status from search_near */
%javaconst(1);
%inline %{
enum SearchStatus { FOUND, NOTFOUND, SMALLER, LARGER };
%}

%extend __wt_cursor {
	%javamethodmodifiers get_key_wrap "protected";
	WT_ITEM *get_key_wrap(JNIEnv *jenv) {
		WT_ITEM k;
		int ret;
		if ((ret = $self->get_key($self, &k)) != 0) {
			throwDbException(jenv, wiredtiger_strerror(ret));
			return NULL;
		}
		return &$self->key;
	}

	%javamethodmodifiers get_value_wrap "protected";
	WT_ITEM *get_value_wrap(JNIEnv *jenv) {
		WT_ITEM v;
		int ret;
		if ((ret = $self->get_value($self, &v)) != 0) {
			throwDbException(jenv, wiredtiger_strerror(ret));
			return NULL;
		}
		return &$self->value;
	}

	%javamethodmodifiers insert_wrap "protected";
	int insert_wrap(WT_ITEM *k, WT_ITEM *v) {
		$self->set_key($self, k);
		$self->set_value($self, v);
		return $self->insert($self);
	}

	%javamethodmodifiers remove_wrap "protected";
	int remove_wrap(WT_ITEM *k) {
		$self->set_key($self, k);
		return $self->remove($self);
	}

	%javamethodmodifiers search_wrap "protected";
	int search_wrap(WT_ITEM *k) {
		$self->set_key($self, k);
		return $self->search($self);
	}

	%javamethodmodifiers search_near_wrap "protected";
	enum SearchStatus search_near_wrap(JNIEnv *jenv, WT_ITEM *k) {
		int cmp, ret;

		$self->set_key($self, k);
		ret = $self->search_near(self, &cmp);
		if (ret != 0 && ret != WT_NOTFOUND)
			throwDbException(jenv, wiredtiger_strerror(ret));
		if (ret == 0)
			return (cmp == 0 ? FOUND : cmp < 0 ? SMALLER : LARGER);
		return (NOTFOUND);
	}

	%javamethodmodifiers update_wrap "protected";
	int update_wrap(WT_ITEM *k, WT_ITEM *v) {
		$self->set_key($self, k);
		$self->set_value($self, v);
		return $self->update($self);
	}
}

%typemap(javacode) struct __wt_cursor %{
	protected byte[] key;
	protected byte[] value;

	public void set_key(String key) {
		this.key = key.getBytes();
	}

	public String get_key() {
		return new String(key);
	}

	public void set_value(byte[] value) {
		this.value = value;
	}

	public byte[] get_value() {
		return this.value;
	}

	public int insert() {
		return insert_wrap(key, value);
	}

	public int update() {
		return update_wrap(key, value);
	}

	public int remove() {
		return remove_wrap(key);
	}

	public int next() {
		int ret = next_wrap();
		key = (ret == 0) ? get_key_wrap() : null;
		value = (ret == 0) ? get_value_wrap() : null;
		return ret;
	}

	public int prev() {
		int ret = prev_wrap();
		key = (ret == 0) ? get_key_wrap() : null;
		value = (ret == 0) ? get_value_wrap() : null;
		return ret;
	}
	public int search() {
		int ret = search_wrap(key);
		key = (ret == 0) ? get_key_wrap() : null;
		value = (ret == 0) ? get_value_wrap() : null;
		return ret;
	}

	public SearchStatus search_near() {
		SearchStatus ret = search_near_wrap(key);
		key = (ret != SearchStatus.NOTFOUND) ? get_key_wrap() : null;
		value = (ret != SearchStatus.NOTFOUND) ? get_value_wrap() : null;
		return ret;
	}
%}

/* Remove / rename parts of the C API that we don't want in Java. */
%immutable __wt_cursor::session;
%immutable __wt_cursor::uri;
%immutable __wt_cursor::key_format;
%immutable __wt_cursor::value_format;
%immutable __wt_session::connection;

%ignore __wt_collator;
%ignore __wt_connection::add_collator;
%ignore __wt_compressor;
%ignore __wt_connection::add_compressor;
%ignore __wt_data_source;
%ignore __wt_connection::add_data_source;
%ignore __wt_event_handler;
%ignore __wt_extractor;
%ignore __wt_connection::add_extractor;
%ignore __wt_item;
%ignore __wt_session::msg_printf;

%ignore wiredtiger_struct_pack;
%ignore wiredtiger_struct_packv;
%ignore wiredtiger_struct_size;
%ignore wiredtiger_struct_sizev;
%ignore wiredtiger_struct_unpack;
%ignore wiredtiger_struct_unpackv;

%ignore wiredtiger_version;

%ignore wiredtiger_extension_init;

%ignore wiredtiger_open;
%rename(open) wiredtiger_open_wrap;
%ignore __wt_connection::open_session;
%rename(open_session) __wt_connection::open_session_wrap;
%ignore __wt_session::open_cursor;
%rename(open_cursor) __wt_session::open_cursor_wrap;

%rename(Cursor) __wt_cursor;
%rename(Session) __wt_session;
%rename(Connection) __wt_connection;

%include "wiredtiger.h"

/* Return new connections, sessions and cursors. */
%inline {
WT_CONNECTION *wiredtiger_open_wrap(JNIEnv *jenv, const char *home, const char *config) {
	WT_CONNECTION *conn = NULL;
	int ret;
	if ((ret = wiredtiger_open(home, NULL, config, &conn)) != 0)
		throwDbException(jenv, wiredtiger_strerror(ret));
	return conn;
}
}

%extend __wt_connection {
	WT_SESSION *open_session_wrap(JNIEnv *jenv, const char *config) {
		WT_SESSION *session = NULL;
		int ret;
		if ((ret = $self->open_session($self, NULL, config, &session)) != 0)
			throwDbException(jenv, wiredtiger_strerror(ret));
		return session;
	}
}

%extend __wt_session {
	WT_CURSOR *open_cursor_wrap(JNIEnv *jenv, const char *uri, WT_CURSOR *to_dup, const char *config) {
		WT_CURSOR *cursor = NULL;
		int ret;
		if ((ret = $self->open_cursor($self, uri, to_dup, config, &cursor)) != 0)
			throwDbException(jenv, wiredtiger_strerror(ret));
		else
			cursor->flags |= WT_CURSTD_RAW;
		return cursor;
	}
}
