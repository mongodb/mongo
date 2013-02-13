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
typedef int bool;

static void throwWiredTigerException(JNIEnv *jenv, const char *msg) {
	jclass excep = (*jenv)->FindClass(jenv, "com/wiredtiger/db/WiredTigerException");
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
		throwWiredTigerException(jenv, wiredtiger_strerror($1));
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
%javamethodmodifiers __wt_cursor::key_format "protected";
%javamethodmodifiers __wt_cursor::value_format "protected";

%ignore __wt_cursor::compare(WT_CURSOR *, WT_CURSOR *, int *);
%rename (compare_wrap) __wt_cursor::compare;

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
			throwWiredTigerException(jenv, wiredtiger_strerror(ret));
			return NULL;
		}
		return &$self->key;
	}

	%javamethodmodifiers get_value_wrap "protected";
	WT_ITEM *get_value_wrap(JNIEnv *jenv) {
		WT_ITEM v;
		int ret;
		if ((ret = $self->get_value($self, &v)) != 0) {
			throwWiredTigerException(jenv, wiredtiger_strerror(ret));
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
			throwWiredTigerException(jenv, wiredtiger_strerror(ret));
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

	int compare_wrap(JNIEnv *jenv, WT_CURSOR *other) {
		int cmp, ret = $self->compare($self, other, &cmp);
		if (ret != 0)
			throwWiredTigerException(jenv, wiredtiger_strerror(ret));
		return cmp;
	}
}

/* Cache key/value formats in Cursor */
%typemap(javabody) struct __wt_cursor %{
 private long swigCPtr;
 protected boolean swigCMemOwn;
 protected String keyFormat;
 protected String valueFormat;
 protected PackOutputStream keyPacker;
 protected PackOutputStream valuePacker;
 protected PackInputStream keyUnpacker;
 protected PackInputStream valueUnpacker;

 protected $javaclassname(long cPtr, boolean cMemoryOwn) {
   swigCMemOwn = cMemoryOwn;
   swigCPtr = cPtr;
   keyFormat = getKey_format();
   valueFormat = getValue_format();
   keyPacker = new PackOutputStream(keyFormat);
   valuePacker = new PackOutputStream(valueFormat);
 }

 protected static long getCPtr($javaclassname obj) {
   return (obj == null) ? 0 : obj.swigCPtr;
 }
%}

%typemap(javacode) struct __wt_cursor %{

        public String getKeyFormat() {
                return keyFormat;
        }
        public String getValueFormat() {
                return valueFormat;
        }

        // Key/value accessors that decode based on format.
        public void addKeyFieldByte(byte value)
        throws WiredTigerPackingException {
                keyPacker.addFieldByte(value);
        }

        public void addKeyFieldByteArray(byte[] value)
        throws WiredTigerPackingException {
                this.addKeyFieldByteArray(value, 0, value.length);
        }

        public void addKeyFieldByteArray(byte[] value, int off, int len)
        throws WiredTigerPackingException {
                keyPacker.addFieldByteArray(value, off, len);
        }

        public void addKeyFieldInt(int value)
        throws WiredTigerPackingException {
                keyPacker.addFieldInt(value);
        }

        public void addKeyFieldLong(long value)
        throws WiredTigerPackingException {
                keyPacker.addFieldLong(value);
        }

        public void addKeyFieldShort(short value)
        throws WiredTigerPackingException {
                keyPacker.addFieldShort(value);
        }

        public void addKeyFieldString(String value)
        throws WiredTigerPackingException {
                keyPacker.addFieldString(value);
        }

        public void addValueFieldByte(byte value)
        throws WiredTigerPackingException {
                valuePacker.addFieldByte(value);
        }

        public void addValueFieldByteArray(byte[] value)
        throws WiredTigerPackingException {
                this.addValueFieldByteArray(value, 0, value.length);
        }

        public void addValueFieldByteArray(byte[] value, int off, int len)
        throws WiredTigerPackingException {
                valuePacker.addFieldByteArray(value, off, len);
        }

        public void addValueFieldInt(int value)
        throws WiredTigerPackingException {
                valuePacker.addFieldInt(value);
        }

        public void addValueFieldLong(long value)
        throws WiredTigerPackingException {
                valuePacker.addFieldLong(value);
        }

        public void addValueFieldShort(short value)
        throws WiredTigerPackingException {
                valuePacker.addFieldShort(value);
        }

        public void addValueFieldString(String value)
        throws WiredTigerPackingException {
                valuePacker.addFieldString(value);
        }

        // TODO: Verify that there is an unpacker available.
        public byte getKeyFieldByte()
        throws WiredTigerPackingException {
                return keyUnpacker.getFieldByte();
        }

        public void getKeyFieldByteArray(byte[] output)
        throws WiredTigerPackingException {
                this.getKeyFieldByteArray(output, 0, output.length);
        }

        public void getKeyFieldByteArray(byte[] output, int off, int len)
        throws WiredTigerPackingException {
                keyUnpacker.getFieldByteArray(output, off, len);
        }

        public byte[] getKeyFieldByteArray()
        throws WiredTigerPackingException {
                return keyUnpacker.getFieldByteArray();
        }

        public int getKeyFieldInt()
        throws WiredTigerPackingException {
                return keyUnpacker.getFieldInt();
        }

        public long getKeyFieldLong()
        throws WiredTigerPackingException {
                return keyUnpacker.getFieldLong();
        }

        public short getKeyFieldShort()
        throws WiredTigerPackingException {
                return keyUnpacker.getFieldShort();
        }

        public String getKeyFieldString()
        throws WiredTigerPackingException {
                return keyUnpacker.getFieldString();
        }

        public byte getValueFieldByte()
        throws WiredTigerPackingException {
                return valueUnpacker.getFieldByte();
        }

        public void getValueFieldByteArray(byte[] output)
        throws WiredTigerPackingException {
                this.getValueFieldByteArray(output, 0, output.length);
        }

        public void getValueFieldByteArray(byte[] output, int off, int len)
        throws WiredTigerPackingException {
                valueUnpacker.getFieldByteArray(output, off, len);
        }

        public byte[] getValueFieldByteArray()
        throws WiredTigerPackingException {
                return valueUnpacker.getFieldByteArray();
        }

        public int getValueFieldInt()
        throws WiredTigerPackingException {
                return valueUnpacker.getFieldInt();
        }

        public long getValueFieldLong()
        throws WiredTigerPackingException {
                return valueUnpacker.getFieldLong();
        }

        public short getValueFieldShort()
        throws WiredTigerPackingException {
                return valueUnpacker.getFieldShort();
        }

        public String getValueFieldString()
        throws WiredTigerPackingException {
                return valueUnpacker.getFieldString();
        }

	public int insert() {
                byte[] key = keyPacker.getValue();
                byte[] value = valuePacker.getValue();
                keyPacker.reset();
                valuePacker.reset();
		return insert_wrap(key, value);
	}

	public int update() {
                byte[] key = keyPacker.getValue();
                byte[] value = valuePacker.getValue();
                keyPacker.reset();
                valuePacker.reset();
		return update_wrap(key, value);
	}

	public int remove() {
                byte[] key = keyPacker.getValue();
                keyPacker.reset();
		return remove_wrap(key);
	}

	public int compare(Cursor other) {
		return compare_wrap(other);
	}

	public int next() {
		int ret = next_wrap();
		keyUnpacker = (ret == 0) ?
                    new PackInputStream(keyFormat, get_key_wrap()) : null;
		valueUnpacker = (ret == 0) ?
                    new PackInputStream(valueFormat, get_value_wrap()) : null;
		return ret;
	}

	public int prev() {
		int ret = prev_wrap();
		keyUnpacker = (ret == 0) ?
                    new PackInputStream(keyFormat, get_key_wrap()) : null;
		valueUnpacker = (ret == 0) ?
                    new PackInputStream(valueFormat, get_value_wrap()) : null;
		return ret;
	}
	public int search() {
		int ret = search_wrap(keyPacker.getValue());
		keyUnpacker = (ret == 0) ?
                    new PackInputStream(keyFormat, get_key_wrap()) : null;
		valueUnpacker = (ret == 0) ?
                    new PackInputStream(valueFormat, get_value_wrap()) : null;
		return ret;
	}

	public SearchStatus search_near() {
		SearchStatus ret = search_near_wrap(keyPacker.getValue());
		keyUnpacker = (ret != SearchStatus.NOTFOUND) ?
                    new PackInputStream(keyFormat, get_key_wrap()) : null;
		valueUnpacker = (ret != SearchStatus.NOTFOUND) ?
                    new PackInputStream(valueFormat, get_value_wrap()) : null;
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
		throwWiredTigerException(jenv, wiredtiger_strerror(ret));
	return conn;
}
}

%extend __wt_connection {
	WT_SESSION *open_session_wrap(JNIEnv *jenv, const char *config) {
		WT_SESSION *session = NULL;
		int ret;
		if ((ret = $self->open_session($self, NULL, config, &session)) != 0)
			throwWiredTigerException(jenv, wiredtiger_strerror(ret));
		return session;
	}
}

%extend __wt_session {
	WT_CURSOR *open_cursor_wrap(JNIEnv *jenv, const char *uri, WT_CURSOR *to_dup, const char *config) {
		WT_CURSOR *cursor = NULL;
		int ret;
		if ((ret = $self->open_cursor($self, uri, to_dup, config, &cursor)) != 0)
			throwWiredTigerException(jenv, wiredtiger_strerror(ret));
		else
			cursor->flags |= WT_CURSTD_RAW;
		return cursor;
	}
}
