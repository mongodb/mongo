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
 *
 * wiredtiger.i
 *	The SWIG interface file defining the wiredtiger Java API.
 */

%module wiredtiger

%include "enums.swg"
%include "typemaps.i"
%include "stdint.i"

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
#include "src/include/wt_internal.h"

/*
 * Closed handle checking:
 *
 * The typedef WT_CURSOR_NULLABLE used in wiredtiger.h is only made
 * visible to the SWIG parser and is used to identify arguments of
 * Cursor type that are permitted to be null.  Likewise, typedefs
 * WT_{CURSOR,SESSION,CONNECTION}_CLOSED identify 'close' calls that
 * need explicit nulling of the swigCPtr.  These typedefs permit
 * special casing in typemaps for input args.
 *
 * We want SWIG to see these 'fake' typenames, but not the compiler.
 */
#define WT_CURSOR_NULLABLE		WT_CURSOR
#define WT_CURSOR_CLOSED		WT_CURSOR
#define WT_SESSION_CLOSED		WT_SESSION
#define WT_CONNECTION_CLOSED		WT_CONNECTION

/*
 * For Connections, Sessions and Cursors created in Java, each of
 * WT_CONNECTION_IMPL, WT_SESSION_IMPL and WT_CURSOR have a
 * lang_private field that store a pointer to a JAVA_CALLBACK, alloced
 * during the various open calls.  {conn,session,cursor}CloseHandler()
 * functions reach into the associated java object, set the swigCPtr
 * to 0, and free the JAVA_CALLBACK. Typemaps matching Connection,
 * Session, Cursor args use the NULL_CHECK macro, which checks if
 * swigCPtr is 0.
 */
typedef struct {
	JavaVM *javavm;		/* Used in async threads to craft a jnienv */
	JNIEnv *jnienv;		/* jni env that created the Session/Cursor */
	WT_SESSION_IMPL *session; /* session used for alloc/free */
	bool cursor_raw;	/* is the cursor opened raw? */
	jobject jobj;		/* the java Session/Cursor/AsyncOp object */
	jobject jcallback;	/* callback object for async ops */
	jfieldID cptr_fid;	/* cached Cursor.swigCPtr field id in session */
	jfieldID asynccptr_fid;	/* cached AsyncOp.swigCptr fid in conn */
	jfieldID kunp_fid;	/* cached AsyncOp.keyUnpacker fid in conn */
	jfieldID vunp_fid;	/* cached AsyncOp.valueUnpacker fid in conn */
	jmethodID notify_mid;	/* cached AsyncCallback.notify mid in conn */
} JAVA_CALLBACK;

static void throwWiredTigerException(JNIEnv *jenv, int err) {
	const char *clname;
	jclass excep;

	clname = NULL;
	excep = NULL;
	if (err == WT_PANIC)
		clname = "com/wiredtiger/db/WiredTigerPanicException";
	else if (err == WT_ROLLBACK)
		clname = "com/wiredtiger/db/WiredTigerRollbackException";
	else
		clname = "com/wiredtiger/db/WiredTigerException";
	if (clname)
		excep = (*jenv)->FindClass(jenv, clname);
	if (excep)
		(*jenv)->ThrowNew(jenv, excep, wiredtiger_strerror(err));
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
%typemap(jni) WT_ITEM, WT_ITEM * "jbyteArray"
%typemap(jtype) WT_ITEM, WT_ITEM * "byte[]"
%typemap(jstype) WT_ITEM, WT_ITEM * "byte[]"

%typemap(javain) WT_ITEM, WT_ITEM * "$javainput"
%typemap(javaout) WT_ITEM, WT_ITEM * {
	return $jnicall;
}

%typemap(in) WT_ITEM * (WT_ITEM item) %{
	$1 = &item;
	$1->data = (*jenv)->GetByteArrayElements(jenv, $input, 0);
	$1->size = (size_t)(*jenv)->GetArrayLength(jenv, $input);
%}

%typemap(argout) WT_ITEM * %{
	(*jenv)->ReleaseByteArrayElements(jenv, $input, (void *)$1->data, 0);
%}

%typemap(out) WT_ITEM %{
	if ($1.data == NULL)
		$result = NULL;
	else if (($result = (*jenv)->NewByteArray(jenv, (jsize)$1.size)) != NULL) {
		(*jenv)->SetByteArrayRegion(jenv,
		    $result, 0, (jsize)$1.size, $1.data);
	}
%}

/* Don't require empty config strings. */
%typemap(default) const char *config %{ $1 = NULL; %}

%typemap(out) int %{
	if ($1 != 0 && $1 != WT_NOTFOUND) {
		throwWiredTigerException(jenv, $1);
		return $null;
	}
	$result = $1;
%}

%define NULL_CHECK(val, name)
	if (!val) {
		SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException,
		#name " is null");
		return $null;
	}
%enddef

/*
 * 'Declare' a WiredTiger class. This sets up boilerplate typemaps.
 */
%define WT_CLASS(type, class, name)
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
%typemap(in, numinputs=0) type *name {
	$1 = *(type **)&jarg1;
	NULL_CHECK($1, $1_name)
}

%typemap(in) class ## _NULLABLE * {
	$1 = *(type **)&$input;
}

%typemap(in) type * {
	$1 = *(type **)&$input;
	NULL_CHECK($1, $1_name)
}

%typemap(javaimports) type "
/**
  * @copydoc class
  * @ingroup wt_java
  */"
%enddef

/*
 * Declare a WT_CLASS so that close methods call a specified closeHandler,
 * after the WT core close function has completed. Arguments to the
 * closeHandler are saved in advance since, as macro args, they may refer to
 * values that are freed/zeroed by the close.
 */
%define WT_CLASS_WITH_CLOSE_HANDLER(type, class, name, closeHandler,
    sess, priv)
WT_CLASS(type, class, name)

/*
 * This typemap recognizes a close function via a special declaration on its
 * first argument. See WT_HANDLE_CLOSED in wiredtiger.h .  Like
 * WT_CURSOR_NULLABLE, the WT_{CURSOR,SESSION,CONNECTION}_CLOSED typedefs
 * are only visible to the SWIG parser.
 */
%typemap(in, numinputs=0) class ## _CLOSED *name (
    WT_SESSION *savesess, JAVA_CALLBACK *jcb) {
	$1 = *(type **)&jarg1;
	NULL_CHECK($1, $1_name)
	savesess = sess;
	jcb = (JAVA_CALLBACK *)(priv);
}

%typemap(freearg, numinputs=0) class ## _CLOSED *name {
	closeHandler(jenv, savesess2, jcb2);
	priv = NULL;
}

%enddef

%pragma(java) moduleimports=%{
/**
 * @defgroup wt_java WiredTiger Java API
 *
 * Java wrappers around the WiredTiger C API.
 */

/**
 * @ingroup wt_java
 */
%}

WT_CLASS_WITH_CLOSE_HANDLER(struct __wt_connection, WT_CONNECTION, connection,
    closeHandler, NULL, ((WT_CONNECTION_IMPL *)$1)->lang_private)
WT_CLASS_WITH_CLOSE_HANDLER(struct __wt_session, WT_SESSION, session,
    closeHandler, $1, ((WT_SESSION_IMPL *)$1)->lang_private)
WT_CLASS_WITH_CLOSE_HANDLER(struct __wt_cursor, WT_CURSOR, cursor,
    cursorCloseHandler, $1->session, ((WT_CURSOR *)$1)->lang_private)
WT_CLASS(struct __wt_async_op, WT_ASYNC_OP, op)

%define COPYDOC(SIGNATURE_CLASS, CLASS, METHOD)
%javamethodmodifiers SIGNATURE_CLASS::METHOD "
  /**
   * @copydoc CLASS::METHOD
   */
  public ";
%enddef

%include "java_doc.i"

/* WT_ASYNC_OP customization. */
/* First, replace the varargs get / set methods with Java equivalents. */
%ignore __wt_async_op::get_key;
%ignore __wt_async_op::get_value;
%ignore __wt_async_op::set_key;
%ignore __wt_async_op::set_value;
%ignore __wt_async_op::insert;
%ignore __wt_async_op::remove;
%ignore __wt_async_op::search;
%ignore __wt_async_op::update;
%immutable __wt_async_op::connection;
%immutable __wt_async_op::key_format;
%immutable __wt_async_op::value_format;

%javamethodmodifiers __wt_async_op::key_format "protected";
%javamethodmodifiers __wt_async_op::value_format "protected";

/* WT_CURSOR customization. */
/* First, replace the varargs get / set methods with Java equivalents. */
%ignore __wt_cursor::get_key;
%ignore __wt_cursor::get_value;
%ignore __wt_cursor::set_key;
%ignore __wt_cursor::set_value;
%ignore __wt_cursor::insert;
%ignore __wt_cursor::remove;
%ignore __wt_cursor::reset;
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
%ignore __wt_cursor::equals(WT_CURSOR *, WT_CURSOR *, int *);
%rename (equals_wrap) __wt_cursor::equals;
%rename (AsyncOpType) WT_ASYNC_OPTYPE;
%rename (getKeyFormat) __wt_async_op::getKey_format;
%rename (getValueFormat) __wt_async_op::getValue_format;
%rename (getType) __wt_async_op::get_type;

/* SWIG magic to turn Java byte strings into data / size. */
%apply (char *STRING, int LENGTH) { (char *data, int size) };

/* Status from search_near */
%javaconst(1);
%inline %{
enum SearchStatus { FOUND, NOTFOUND, SMALLER, LARGER };
%}

%wrapper %{
/* Zero out SWIG's pointer to the C object,
 * equivalent to 'jobj.swigCPtr = 0;' in java.
 * We expect that either env in non-null (if called
 * via an explicit session/cursor close() call), or
 * that session is non-null (if called implicitly
 * as part of connection/session close).
 */
static int
javaClose(JNIEnv *env, WT_SESSION *session, JAVA_CALLBACK *jcb, jfieldID *pfid)
{
	jclass cls;
	jfieldID fid;
	WT_CONNECTION_IMPL *conn;

	/* If we were not called via an implicit close call,
	 * we won't have a JNIEnv yet.  Get one from the connection,
	 * since the thread that started the session may have
	 * terminated.
	 */
	if (env == NULL) {
		conn = (WT_CONNECTION_IMPL *)session->connection;
		env = ((JAVA_CALLBACK *)conn->lang_private)->jnienv;
	}
	if (pfid == NULL || *pfid == NULL) {
		cls = (*env)->GetObjectClass(env, jcb->jobj);
		fid = (*env)->GetFieldID(env, cls, "swigCPtr", "J");
		if (pfid != NULL)
			*pfid = fid;
	} else
		fid = *pfid;

	(*env)->SetLongField(env, jcb->jobj, fid, 0L);
	(*env)->DeleteGlobalRef(env, jcb->jobj);
	__wt_free(jcb->session, jcb);
	return (0);
}

/* Connection and Session close handler. */
static int
closeHandler(JNIEnv *env, WT_SESSION *session, JAVA_CALLBACK *jcb)
{
	return (javaClose(env, session, jcb, NULL));
}

/* Cursor specific close handler. */
static int
cursorCloseHandler(JNIEnv *env, WT_SESSION *wt_session, JAVA_CALLBACK *jcb)
{
	int ret;
	JAVA_CALLBACK *sess_jcb;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	sess_jcb = (JAVA_CALLBACK *)session->lang_private;
	ret = javaClose(env, wt_session, jcb,
	    sess_jcb ? &sess_jcb->cptr_fid : NULL);

	return (ret);
}

/* Add event handler support. */
static int
javaCloseHandler(WT_EVENT_HANDLER *handler, WT_SESSION *session,
	WT_CURSOR *cursor)
{
	int ret;
	JAVA_CALLBACK *jcb;

	WT_UNUSED(handler);

	ret = 0;
	if (cursor != NULL) {
		if ((jcb = (JAVA_CALLBACK *)cursor->lang_private) != NULL) {
			ret = cursorCloseHandler(NULL, session, jcb);
			cursor->lang_private = NULL;
		}
	} else if ((jcb = ((WT_SESSION_IMPL *)session)->lang_private) != NULL) {
		ret = closeHandler(NULL, session, jcb);
		((WT_SESSION_IMPL *)session)->lang_private = NULL;
	}
	return (ret);
}

WT_EVENT_HANDLER javaApiEventHandler = {NULL, NULL, NULL, javaCloseHandler};

static int
javaAsyncHandler(WT_ASYNC_CALLBACK *cb, WT_ASYNC_OP *asyncop, int opret,
    uint32_t flags)
{
	int ret, envret;
	JAVA_CALLBACK *jcb, *conn_jcb;
	JavaVM *javavm;
	jclass cls;
	jfieldID fid;
	jmethodID mid;
	jobject jcallback;
	JNIEnv *jenv;
	WT_ASYNC_OP_IMPL *op;
	WT_SESSION_IMPL *session;

	WT_UNUSED(cb);
	WT_UNUSED(flags);
	op = (WT_ASYNC_OP_IMPL *)asyncop;
	session = O2S(op);
	jcb = (JAVA_CALLBACK *)asyncop->c.lang_private;
	conn_jcb = (JAVA_CALLBACK *)S2C(session)->lang_private;
	asyncop->c.lang_private = NULL;
	jcallback = jcb->jcallback;

	/*
	 * We rely on the fact that the async machinery uses a pool of
	 * threads.  Here we attach the current native (POSIX)
	 * thread to a Java thread and never detach it.  If the native
	 * thread was previously seen by this callback, it will be
	 * attached to the same Java thread as before without
	 * incurring the cost of the thread initialization.
	 * Marking the Java thread as a daemon means its existence
	 * won't keep an application from exiting.
	 */
	javavm = jcb->javavm;
	envret = (*javavm)->GetEnv(javavm, (void **)&jenv, JNI_VERSION_1_6);
	if (envret == JNI_EDETACHED) {
		if ((*javavm)->AttachCurrentThreadAsDaemon(javavm,
		    (void **)&jenv, NULL) != 0) {
			ret = EBUSY;
			goto err;
		}
	} else if (envret != JNI_OK) {
		ret = EBUSY;
		goto err;
	}

	/*
	 * Look up any needed field and method ids, and cache them
	 * in the connection's lang_private.  fid and mids are
	 * stable.
	 */
	if (conn_jcb->notify_mid == NULL) {
		/* Any JNI error until the actual callback is unexpected. */
		ret = EINVAL;

		cls = (*jenv)->GetObjectClass(jenv, jcb->jobj);
		if (cls == NULL)
			goto err;
		fid = (*jenv)->GetFieldID(jenv, cls,
		    "keyUnpacker", "Lcom/wiredtiger/db/PackInputStream;");
		if (fid == NULL)
			goto err;
		conn_jcb->kunp_fid = fid;

		fid = (*jenv)->GetFieldID(jenv, cls,
		    "valueUnpacker", "Lcom/wiredtiger/db/PackInputStream;");
		if (fid == NULL)
			goto err;
		conn_jcb->vunp_fid = fid;

		cls = (*jenv)->GetObjectClass(jenv, jcallback);
		if (cls == NULL)
			goto err;
		mid = (*jenv)->GetMethodID(jenv, cls, "notify",
		    "(Lcom/wiredtiger/db/AsyncOp;II)I");
		if (mid == NULL)
			goto err;
		conn_jcb->notify_mid = mid;
	}

	/*
	 * Invalidate the unpackers so any calls to op.getKey()
	 * and op.getValue get fresh results.
	 */
	(*jenv)->SetObjectField(jenv, jcb->jobj, conn_jcb->kunp_fid, NULL);
	(*jenv)->SetObjectField(jenv, jcb->jobj, conn_jcb->vunp_fid, NULL);

	/* Call the registered callback. */
	ret = (*jenv)->CallIntMethod(jenv, jcallback, conn_jcb->notify_mid,
	    jcb->jobj, opret, flags);

	if ((*jenv)->ExceptionOccurred(jenv)) {
		(*jenv)->ExceptionDescribe(jenv);
		(*jenv)->ExceptionClear(jenv);
	}
	if (0) {
err:		__wt_err(session, ret, "Java async callback error");
	}

	/* Invalidate the AsyncOp, further use throws NullPointerException. */
	ret = javaClose(jenv, NULL, jcb, &conn_jcb->asynccptr_fid);

	(*jenv)->DeleteGlobalRef(jenv, jcallback);

	if (ret == 0 && (opret == 0 || opret == WT_NOTFOUND))
		return (0);
	else
		return (1);
}

WT_ASYNC_CALLBACK javaApiAsyncHandler = {javaAsyncHandler};
%}

%extend __wt_async_op {

	%javamethodmodifiers get_key_wrap "protected";
	WT_ITEM get_key_wrap(JNIEnv *jenv) {
		WT_ITEM k;
		int ret;
		k.data = NULL;
		if ((ret = $self->get_key($self, &k)) != 0)
			throwWiredTigerException(jenv, ret);
		return k;
	}

	%javamethodmodifiers get_value_wrap "protected";
	WT_ITEM get_value_wrap(JNIEnv *jenv) {
		WT_ITEM v;
		int ret;
		v.data = NULL;
		if ((ret = $self->get_value($self, &v)) != 0)
			throwWiredTigerException(jenv, ret);
		return v;
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

	%javamethodmodifiers update_wrap "protected";
	int update_wrap(WT_ITEM *k, WT_ITEM *v) {
		$self->set_key($self, k);
		$self->set_value($self, v);
		return $self->update($self);
	}

	%javamethodmodifiers _java_raw "protected";
	bool _java_raw(JNIEnv *jenv) {
		(void)jenv;
		JAVA_CALLBACK *jcb = (JAVA_CALLBACK *)$self->c.lang_private;
		return jcb->cursor_raw;
	}

	%javamethodmodifiers _java_init "protected";
	int _java_init(jobject jasyncop) {
		JAVA_CALLBACK *jcb =
		    (JAVA_CALLBACK *)$self->c.lang_private;
		jcb->jobj = JCALL1(NewGlobalRef, jcb->jnienv, jasyncop);
		JCALL1(DeleteLocalRef, jcb->jnienv, jasyncop);
		return (0);
	}
}

/* Cache key/value formats in Async_op */
%typemap(javabody) struct __wt_async_op %{
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
   wiredtigerJNI.AsyncOp__java_init(swigCPtr, this, this);
 }

 protected static long getCPtr($javaclassname obj) {
   return (obj == null) ? 0 : obj.swigCPtr;
 }
%}

%typemap(javacode) struct __wt_async_op %{

	/**
	 * Retrieve the format string for this async_op's key.
	 */
	public String getKeyFormat() {
		return keyFormat;
	}

	/**
	 * Retrieve the format string for this async_op's value.
	 */
	public String getValueFormat() {
		return valueFormat;
	}

	/**
	 * Append a byte to the async_op's key.
	 *
	 * \param value The value to append.
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putKeyByte(byte value)
	throws WiredTigerPackingException {
		keyUnpacker = null;
		keyPacker.addByte(value);
		return this;
	}

	/**
	 * Append a byte array to the async_op's key.
	 *
	 * \param value The value to append.
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putKeyByteArray(byte[] value)
	throws WiredTigerPackingException {
		this.putKeyByteArray(value, 0, value.length);
		return this;
	}

	/**
	 * Append a byte array to the async_op's key.
	 *
	 * \param value The value to append.
	 * \param off The offset into value at which to start.
	 * \param len The length of the byte array.
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putKeyByteArray(byte[] value, int off, int len)
	throws WiredTigerPackingException {
		keyUnpacker = null;
		keyPacker.addByteArray(value, off, len);
		return this;
	}

	/**
	 * Append an integer to the async_op's key.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putKeyInt(int value)
	throws WiredTigerPackingException {
		keyUnpacker = null;
		keyPacker.addInt(value);
		return this;
	}

	/**
	 * Append a long to the async_op's key.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putKeyLong(long value)
	throws WiredTigerPackingException {
		keyUnpacker = null;
		keyPacker.addLong(value);
		return this;
	}

	/**
	 * Append a record number to the async_op's key.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putKeyRecord(long value)
	throws WiredTigerPackingException {
		keyUnpacker = null;
		keyPacker.addRecord(value);
		return this;
	}

	/**
	 * Append a short integer to the async_op's key.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putKeyShort(short value)
	throws WiredTigerPackingException {
		keyUnpacker = null;
		keyPacker.addShort(value);
		return this;
	}

	/**
	 * Append a string to the async_op's key.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putKeyString(String value)
	throws WiredTigerPackingException {
		keyUnpacker = null;
		keyPacker.addString(value);
		return this;
	}

	/**
	 * Append a byte to the async_op's value.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putValueByte(byte value)
	throws WiredTigerPackingException {
		valueUnpacker = null;
		valuePacker.addByte(value);
		return this;
	}

	/**
	 * Append a byte array to the async_op's value.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putValueByteArray(byte[] value)
	throws WiredTigerPackingException {
		this.putValueByteArray(value, 0, value.length);
		return this;
	}

	/**
	 * Append a byte array to the async_op's value.
	 *
	 * \param value The value to append
	 * \param off The offset into value at which to start.
	 * \param len The length of the byte array.
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putValueByteArray(byte[] value, int off, int len)
	throws WiredTigerPackingException {
		valueUnpacker = null;
		valuePacker.addByteArray(value, off, len);
		return this;
	}

	/**
	 * Append an integer to the async_op's value.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putValueInt(int value)
	throws WiredTigerPackingException {
		valueUnpacker = null;
		valuePacker.addInt(value);
		return this;
	}

	/**
	 * Append a long to the async_op's value.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putValueLong(long value)
	throws WiredTigerPackingException {
		valueUnpacker = null;
		valuePacker.addLong(value);
		return this;
	}

	/**
	 * Append a record number to the async_op's value.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putValueRecord(long value)
	throws WiredTigerPackingException {
		valueUnpacker = null;
		valuePacker.addRecord(value);
		return this;
	}

	/**
	 * Append a short integer to the async_op's value.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putValueShort(short value)
	throws WiredTigerPackingException {
		valueUnpacker = null;
		valuePacker.addShort(value);
		return this;
	}

	/**
	 * Append a string to the async_op's value.
	 *
	 * \param value The value to append
	 * \return This async_op object, so put calls can be chained.
	 */
	public AsyncOp putValueString(String value)
	throws WiredTigerPackingException {
		valueUnpacker = null;
		valuePacker.addString(value);
		return this;
	}

	/**
	 * Retrieve a byte from the async_op's key.
	 *
	 * \return The requested value.
	 */
	public byte getKeyByte()
	throws WiredTigerPackingException {
		return getKeyUnpacker().getByte();
	}

	/**
	 * Retrieve a byte array from the async_op's key.
	 *
	 * \param output The byte array where the returned value will be stored.
	 *	       The array should be large enough to store the entire
	 *	       data item, if not a truncated value will be returned.
	 */
	public void getKeyByteArray(byte[] output)
	throws WiredTigerPackingException {
		this.getKeyByteArray(output, 0, output.length);
	}

	/**
	 * Retrieve a byte array from the async_op's key.
	 *
	 * \param output The byte array where the returned value will be stored.
	 * \param off Offset into the destination buffer to start copying into.
	 * \param len The length should be large enough to store the entire
	 *	      data item, if not a truncated value will be returned.
	 */
	public void getKeyByteArray(byte[] output, int off, int len)
	throws WiredTigerPackingException {
		getKeyUnpacker().getByteArray(output, off, len);
	}

	/**
	 * Retrieve a byte array from the async_op's key.
	 *
	 * \return The requested value.
	 */
	public byte[] getKeyByteArray()
	throws WiredTigerPackingException {
		return getKeyUnpacker().getByteArray();
	}

	/**
	 * Retrieve an integer from the async_op's key.
	 *
	 * \return The requested value.
	 */
	public int getKeyInt()
	throws WiredTigerPackingException {
		return getKeyUnpacker().getInt();
	}

	/**
	 * Retrieve a long from the async_op's key.
	 *
	 * \return The requested value.
	 */
	public long getKeyLong()
	throws WiredTigerPackingException {
		return getKeyUnpacker().getLong();
	}

	/**
	 * Retrieve a record number from the async_op's key.
	 *
	 * \return The requested value.
	 */
	public long getKeyRecord()
	throws WiredTigerPackingException {
		return getKeyUnpacker().getRecord();
	}

	/**
	 * Retrieve a short integer from the async_op's key.
	 *
	 * \return The requested value.
	 */
	public short getKeyShort()
	throws WiredTigerPackingException {
		return getKeyUnpacker().getShort();
	}

	/**
	 * Retrieve a string from the async_op's key.
	 *
	 * \return The requested value.
	 */
	public String getKeyString()
	throws WiredTigerPackingException {
		return getKeyUnpacker().getString();
	}

	/**
	 * Retrieve a byte from the async_op's value.
	 *
	 * \return The requested value.
	 */
	public byte getValueByte()
	throws WiredTigerPackingException {
		return getValueUnpacker().getByte();
	}

	/**
	 * Retrieve a byte array from the async_op's value.
	 *
	 * \param output The byte array where the returned value will be stored.
	 *	       The array should be large enough to store the entire
	 *	       data item, if not a truncated value will be returned.
	 */
	public void getValueByteArray(byte[] output)
	throws WiredTigerPackingException {
		this.getValueByteArray(output, 0, output.length);
	}

	/**
	 * Retrieve a byte array from the async_op's value.
	 *
	 * \param output The byte array where the returned value will be stored.
	 * \param off Offset into the destination buffer to start copying into.
	 * \param len The length should be large enough to store the entire
	 *	      data item, if not a truncated value will be returned.
	 */
	public void getValueByteArray(byte[] output, int off, int len)
	throws WiredTigerPackingException {
		getValueUnpacker().getByteArray(output, off, len);
	}

	/**
	 * Retrieve a byte array from the async_op's value.
	 *
	 * \return The requested value.
	 */
	public byte[] getValueByteArray()
	throws WiredTigerPackingException {
		return getValueUnpacker().getByteArray();
	}

	/**
	 * Retrieve an integer from the async_op's value.
	 *
	 * \return The requested value.
	 */
	public int getValueInt()
	throws WiredTigerPackingException {
		return getValueUnpacker().getInt();
	}

	/**
	 * Retrieve a long from the async_op's value.
	 *
	 * \return The requested value.
	 */
	public long getValueLong()
	throws WiredTigerPackingException {
		return getValueUnpacker().getLong();
	}

	/**
	 * Retrieve a record number from the async_op's value.
	 *
	 * \return The requested value.
	 */
	public long getValueRecord()
	throws WiredTigerPackingException {
		return getValueUnpacker().getRecord();
	}

	/**
	 * Retrieve a short integer from the async_op's value.
	 *
	 * \return The requested value.
	 */
	public short getValueShort()
	throws WiredTigerPackingException {
		return getValueUnpacker().getShort();
	}

	/**
	 * Retrieve a string from the async_op's value.
	 *
	 * \return The requested value.
	 */
	public String getValueString()
	throws WiredTigerPackingException {
		return getValueUnpacker().getString();
	}

	/**
	 * Insert the async_op's current key/value into the table.
	 *
	 * \return The status of the operation.
	 */
	public int insert()
	throws WiredTigerException {
		byte[] key = keyPacker.getValue();
		byte[] value = valuePacker.getValue();
		keyPacker.reset();
		valuePacker.reset();
		return insert_wrap(key, value);
	}

	/**
	 * Update the async_op's current key/value into the table.
	 *
	 * \return The status of the operation.
	 */
	public int update()
	throws WiredTigerException {
		byte[] key = keyPacker.getValue();
		byte[] value = valuePacker.getValue();
		keyPacker.reset();
		valuePacker.reset();
		return update_wrap(key, value);
	}

	/**
	 * Remove the async_op's current key/value into the table.
	 *
	 * \return The status of the operation.
	 */
	public int remove()
	throws WiredTigerException {
		byte[] key = keyPacker.getValue();
		keyPacker.reset();
		return remove_wrap(key);
	}

	/**
	 * Search for an item in the table.
	 *
	 * \return The result of the comparison.
	 */
	public int search()
	throws WiredTigerException {
		int ret = search_wrap(keyPacker.getValue());
		keyPacker.reset();
		valuePacker.reset();
		return ret;
	}

	/**
	 * Set up the key unpacker or return previously cached value.
	 *
	 * \return The key unpacker.
	 */
	private PackInputStream getKeyUnpacker()
	throws WiredTigerPackingException {
		if (keyUnpacker == null)
			keyUnpacker =
			    new PackInputStream(keyFormat, get_key_wrap(),
			    _java_raw());
		return keyUnpacker;
	}

	/**
	 * Set up the value unpacker or return previously cached value.
	 *
	 * \return The value unpacker.
	 */
	private PackInputStream getValueUnpacker()
	throws WiredTigerPackingException {
		if (valueUnpacker == null)
			valueUnpacker =
			    new PackInputStream(valueFormat, get_value_wrap(),
			    _java_raw());
		return valueUnpacker;
	}

%}

%extend __wt_cursor {

	%javamethodmodifiers get_key_wrap "protected";
	WT_ITEM get_key_wrap(JNIEnv *jenv) {
		WT_ITEM k;
		int ret;
		k.data = NULL;
		if ((ret = $self->get_key($self, &k)) != 0)
			throwWiredTigerException(jenv, ret);
		return k;
	}

	%javamethodmodifiers get_value_wrap "protected";
	WT_ITEM get_value_wrap(JNIEnv *jenv) {
		WT_ITEM v;
		int ret;
		v.data = NULL;
		if ((ret = $self->get_value($self, &v)) != 0)
			throwWiredTigerException(jenv, ret);
		return v;
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

	%javamethodmodifiers reset_wrap "protected";
	int reset_wrap() {
		return $self->reset($self);
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
			throwWiredTigerException(jenv, ret);
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

	%javamethodmodifiers compare_wrap "protected";
	int compare_wrap(JNIEnv *jenv, WT_CURSOR *other) {
		int cmp, ret = $self->compare($self, other, &cmp);
		if (ret != 0)
			throwWiredTigerException(jenv, ret);
		return cmp;
	}

	%javamethodmodifiers equals_wrap "protected";
	int equals_wrap(JNIEnv *jenv, WT_CURSOR *other) {
		int cmp, ret = $self->equals($self, other, &cmp);
		if (ret != 0)
			throwWiredTigerException(jenv, ret);
		return cmp;
	}

	%javamethodmodifiers _java_raw "protected";
	bool _java_raw(JNIEnv *jenv) {
		(void)jenv;
		JAVA_CALLBACK *jcb = (JAVA_CALLBACK *)$self->lang_private;
		return jcb->cursor_raw;
	}

	%javamethodmodifiers _java_init "protected";
	int _java_init(jobject jcursor) {
		JAVA_CALLBACK *jcb = (JAVA_CALLBACK *)$self->lang_private;
		jcb->jobj = JCALL1(NewGlobalRef, jcb->jnienv, jcursor);
		JCALL1(DeleteLocalRef, jcb->jnienv, jcursor);
		return (0);
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
   wiredtigerJNI.Cursor__java_init(swigCPtr, this, this);
 }

 protected static long getCPtr($javaclassname obj) {
   return (obj == null) ? 0 : obj.swigCPtr;
 }
%}

%typemap(javacode) struct __wt_cursor %{

	/**
	 * Retrieve the format string for this cursor's key.
	 */
	public String getKeyFormat() {
		return keyFormat;
	}

	/**
	 * Retrieve the format string for this cursor's value.
	 */
	public String getValueFormat() {
		return valueFormat;
	}

	/**
	 * Append a byte to the cursor's key.
	 *
	 * \param value The value to append.
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putKeyByte(byte value)
	throws WiredTigerPackingException {
		keyPacker.addByte(value);
		return this;
	}

	/**
	 * Append a byte array to the cursor's key.
	 *
	 * \param value The value to append.
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putKeyByteArray(byte[] value)
	throws WiredTigerPackingException {
		this.putKeyByteArray(value, 0, value.length);
		return this;
	}

	/**
	 * Append a byte array to the cursor's key.
	 *
	 * \param value The value to append.
	 * \param off The offset into value at which to start.
	 * \param len The length of the byte array.
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putKeyByteArray(byte[] value, int off, int len)
	throws WiredTigerPackingException {
		keyPacker.addByteArray(value, off, len);
		return this;
	}

	/**
	 * Append an integer to the cursor's key.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putKeyInt(int value)
	throws WiredTigerPackingException {
		keyPacker.addInt(value);
		return this;
	}

	/**
	 * Append a long to the cursor's key.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putKeyLong(long value)
	throws WiredTigerPackingException {
		keyPacker.addLong(value);
		return this;
	}

	/**
	 * Append a record number to the cursor's key.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putKeyRecord(long value)
	throws WiredTigerPackingException {
		keyPacker.addRecord(value);
		return this;
	}

	/**
	 * Append a short integer to the cursor's key.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putKeyShort(short value)
	throws WiredTigerPackingException {
		keyPacker.addShort(value);
		return this;
	}

	/**
	 * Append a string to the cursor's key.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putKeyString(String value)
	throws WiredTigerPackingException {
		keyPacker.addString(value);
		return this;
	}

	/**
	 * Append a byte to the cursor's value.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putValueByte(byte value)
	throws WiredTigerPackingException {
		valuePacker.addByte(value);
		return this;
	}

	/**
	 * Append a byte array to the cursor's value.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putValueByteArray(byte[] value)
	throws WiredTigerPackingException {
		this.putValueByteArray(value, 0, value.length);
		return this;
	}

	/**
	 * Append a byte array to the cursor's value.
	 *
	 * \param value The value to append
	 * \param off The offset into value at which to start.
	 * \param len The length of the byte array.
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putValueByteArray(byte[] value, int off, int len)
	throws WiredTigerPackingException {
		valuePacker.addByteArray(value, off, len);
		return this;
	}

	/**
	 * Append an integer to the cursor's value.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putValueInt(int value)
	throws WiredTigerPackingException {
		valuePacker.addInt(value);
		return this;
	}

	/**
	 * Append a long to the cursor's value.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putValueLong(long value)
	throws WiredTigerPackingException {
		valuePacker.addLong(value);
		return this;
	}

	/**
	 * Append a record number to the cursor's value.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putValueRecord(long value)
	throws WiredTigerPackingException {
		valuePacker.addRecord(value);
		return this;
	}

	/**
	 * Append a short integer to the cursor's value.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putValueShort(short value)
	throws WiredTigerPackingException {
		valuePacker.addShort(value);
		return this;
	}

	/**
	 * Append a string to the cursor's value.
	 *
	 * \param value The value to append
	 * \return This cursor object, so put calls can be chained.
	 */
	public Cursor putValueString(String value)
	throws WiredTigerPackingException {
		valuePacker.addString(value);
		return this;
	}

	/**
	 * Retrieve a byte from the cursor's key.
	 *
	 * \return The requested value.
	 */
	public byte getKeyByte()
	throws WiredTigerPackingException {
		return keyUnpacker.getByte();
	}

	/**
	 * Retrieve a byte array from the cursor's key.
	 *
	 * \param output The byte array where the returned value will be stored.
	 *	       The array should be large enough to store the entire
	 *	       data item, if not a truncated value will be returned.
	 */
	public void getKeyByteArray(byte[] output)
	throws WiredTigerPackingException {
		this.getKeyByteArray(output, 0, output.length);
	}

	/**
	 * Retrieve a byte array from the cursor's key.
	 *
	 * \param output The byte array where the returned value will be stored.
	 * \param off Offset into the destination buffer to start copying into.
	 * \param len The length should be large enough to store the entire
	 *	      data item, if not a truncated value will be returned.
	 */
	public void getKeyByteArray(byte[] output, int off, int len)
	throws WiredTigerPackingException {
		keyUnpacker.getByteArray(output, off, len);
	}

	/**
	 * Retrieve a byte array from the cursor's key.
	 *
	 * \return The requested value.
	 */
	public byte[] getKeyByteArray()
	throws WiredTigerPackingException {
		return keyUnpacker.getByteArray();
	}

	/**
	 * Retrieve an integer from the cursor's key.
	 *
	 * \return The requested value.
	 */
	public int getKeyInt()
	throws WiredTigerPackingException {
		return keyUnpacker.getInt();
	}

	/**
	 * Retrieve a long from the cursor's key.
	 *
	 * \return The requested value.
	 */
	public long getKeyLong()
	throws WiredTigerPackingException {
		return keyUnpacker.getLong();
	}

	/**
	 * Retrieve a record number from the cursor's key.
	 *
	 * \return The requested value.
	 */
	public long getKeyRecord()
	throws WiredTigerPackingException {
		return keyUnpacker.getRecord();
	}

	/**
	 * Retrieve a short integer from the cursor's key.
	 *
	 * \return The requested value.
	 */
	public short getKeyShort()
	throws WiredTigerPackingException {
		return keyUnpacker.getShort();
	}

	/**
	 * Retrieve a string from the cursor's key.
	 *
	 * \return The requested value.
	 */
	public String getKeyString()
	throws WiredTigerPackingException {
		return keyUnpacker.getString();
	}

	/**
	 * Retrieve a byte from the cursor's value.
	 *
	 * \return The requested value.
	 */
	public byte getValueByte()
	throws WiredTigerPackingException {
		return valueUnpacker.getByte();
	}

	/**
	 * Retrieve a byte array from the cursor's value.
	 *
	 * \param output The byte array where the returned value will be stored.
	 *	       The array should be large enough to store the entire
	 *	       data item, if not a truncated value will be returned.
	 */
	public void getValueByteArray(byte[] output)
	throws WiredTigerPackingException {
		this.getValueByteArray(output, 0, output.length);
	}

	/**
	 * Retrieve a byte array from the cursor's value.
	 *
	 * \param output The byte array where the returned value will be stored.
	 * \param off Offset into the destination buffer to start copying into.
	 * \param len The length should be large enough to store the entire
	 *	      data item, if not a truncated value will be returned.
	 */
	public void getValueByteArray(byte[] output, int off, int len)
	throws WiredTigerPackingException {
		valueUnpacker.getByteArray(output, off, len);
	}

	/**
	 * Retrieve a byte array from the cursor's value.
	 *
	 * \return The requested value.
	 */
	public byte[] getValueByteArray()
	throws WiredTigerPackingException {
		return valueUnpacker.getByteArray();
	}

	/**
	 * Retrieve an integer from the cursor's value.
	 *
	 * \return The requested value.
	 */
	public int getValueInt()
	throws WiredTigerPackingException {
		return valueUnpacker.getInt();
	}

	/**
	 * Retrieve a long from the cursor's value.
	 *
	 * \return The requested value.
	 */
	public long getValueLong()
	throws WiredTigerPackingException {
		return valueUnpacker.getLong();
	}

	/**
	 * Retrieve a record number from the cursor's value.
	 *
	 * \return The requested value.
	 */
	public long getValueRecord()
	throws WiredTigerPackingException {
		return valueUnpacker.getRecord();
	}

	/**
	 * Retrieve a short integer from the cursor's value.
	 *
	 * \return The requested value.
	 */
	public short getValueShort()
	throws WiredTigerPackingException {
		return valueUnpacker.getShort();
	}

	/**
	 * Retrieve a string from the cursor's value.
	 *
	 * \return The requested value.
	 */
	public String getValueString()
	throws WiredTigerPackingException {
		return valueUnpacker.getString();
	}

	/**
	 * Insert the cursor's current key/value into the table.
	 *
	 * \return The status of the operation.
	 */
	public int insert()
	throws WiredTigerException {
		byte[] key = keyPacker.getValue();
		byte[] value = valuePacker.getValue();
		keyPacker.reset();
		valuePacker.reset();
		return insert_wrap(key, value);
	}

	/**
	 * Update the cursor's current key/value into the table.
	 *
	 * \return The status of the operation.
	 */
	public int update()
	throws WiredTigerException {
		byte[] key = keyPacker.getValue();
		byte[] value = valuePacker.getValue();
		keyPacker.reset();
		valuePacker.reset();
		return update_wrap(key, value);
	}

	/**
	 * Remove the cursor's current key/value into the table.
	 *
	 * \return The status of the operation.
	 */
	public int remove()
	throws WiredTigerException {
		byte[] key = keyPacker.getValue();
		keyPacker.reset();
		return remove_wrap(key);
	}

	/**
	 * Compare this cursor's position to another Cursor.
	 *
	 * \return The result of the comparison.
	 */
	public int compare(Cursor other)
	throws WiredTigerException {
		return compare_wrap(other);
	}

	/**
	 * Compare this cursor's position to another Cursor.
	 *
	 * \return The result of the comparison.
	 */
	public int equals(Cursor other)
	throws WiredTigerException {
		return equals_wrap(other);
	}

	/**
	 * Retrieve the next item in the table.
	 *
	 * \return The result of the comparison.
	 */
	public int next()
	throws WiredTigerException {
		int ret = next_wrap();
		keyPacker.reset();
		valuePacker.reset();
		keyUnpacker = initKeyUnpacker(ret == 0);
		valueUnpacker = initValueUnpacker(ret == 0);
		return ret;
	}

	/**
	 * Retrieve the previous item in the table.
	 *
	 * \return The result of the comparison.
	 */
	public int prev()
	throws WiredTigerException {
		int ret = prev_wrap();
		keyPacker.reset();
		valuePacker.reset();
		keyUnpacker = initKeyUnpacker(ret == 0);
		valueUnpacker = initValueUnpacker(ret == 0);
		return ret;
	}

	/**
	 * Reset a cursor.
	 *
	 * \return The status of the operation.
	 */
	public int reset()
	throws WiredTigerException {
		int ret = reset_wrap();
		keyPacker.reset();
		valuePacker.reset();
		keyUnpacker = null;
		valueUnpacker = null;
		return ret;
	}

	/**
	 * Search for an item in the table.
	 *
	 * \return The result of the comparison.
	 */
	public int search()
	throws WiredTigerException {
		int ret = search_wrap(keyPacker.getValue());
		keyPacker.reset();
		valuePacker.reset();
		keyUnpacker = initKeyUnpacker(ret == 0);
		valueUnpacker = initValueUnpacker(ret == 0);
		return ret;
	}

	/**
	 * Search for an item in the table.
	 *
	 * \return The result of the comparison.
	 */
	public SearchStatus search_near()
	throws WiredTigerException {
		SearchStatus ret = search_near_wrap(keyPacker.getValue());
		keyPacker.reset();
		valuePacker.reset();
		keyUnpacker = initKeyUnpacker(ret != SearchStatus.NOTFOUND);
		valueUnpacker = initValueUnpacker(ret != SearchStatus.NOTFOUND);
		return ret;
	}

	/**
	 * Initialize a key unpacker after an operation that changes
	 * the cursor position.
	 *
	 * \param success Whether the associated operation succeeded.
	 * \return The key unpacker.
	 */
	private PackInputStream initKeyUnpacker(boolean success)
	throws WiredTigerException {
		if (!success || keyFormat.equals(""))
			return null;
		else
			return new PackInputStream(keyFormat,
			    get_key_wrap(), _java_raw());
	}

	/**
	 * Initialize a value unpacker after an operation that changes
	 * the cursor position.
	 *
	 * \param success Whether the associated operation succeeded.
	 * \return The value unpacker.
	 */
	private PackInputStream initValueUnpacker(boolean success)
	throws WiredTigerException {
		if (!success || valueFormat.equals(""))
			return null;
		else
			return new PackInputStream(valueFormat,
			    get_value_wrap(), _java_raw());
	}
%}

/* Put a WiredTigerException on all wrapped methods. We'd like this
 * to only apply to methods returning int.  SWIG doesn't have a way
 * to do this, so we remove the exception for simple getters and such.
 */
%javaexception("com.wiredtiger.db.WiredTigerException") { $action; }
%javaexception("") wiredtiger_strerror { $action; }
%javaexception("") __wt_async_op::_java_raw { $action; }
%javaexception("") __wt_async_op::connection { $action; }
%javaexception("") __wt_async_op::get_type { $action; }
%javaexception("") __wt_async_op::get_id { $action; }
%javaexception("") __wt_async_op::key_format { $action; }
%javaexception("") __wt_async_op::value_format { $action; }
%javaexception("") __wt_connection::_java_init { $action; }
%javaexception("") __wt_connection::get_home { $action; }
%javaexception("") __wt_connection::is_new { $action; }
%javaexception("") __wt_cursor::_java_raw { $action; }
%javaexception("") __wt_cursor::key_format { $action; }
%javaexception("") __wt_cursor::session { $action; }
%javaexception("") __wt_cursor::uri { $action; }
%javaexception("") __wt_cursor::value_format { $action; }
%javaexception("") __wt_session::_java_init { $action; }
%javaexception("") __wt_session::connection { $action; }

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
%ignore __wt_encryptor;
%ignore __wt_connection::add_encryptor;
%ignore __wt_event_handler;
%ignore __wt_extractor;
%ignore __wt_connection::add_extractor;
%ignore __wt_file_system;
%ignore __wt_file_handle;
%ignore __wt_connection::set_file_system;
%ignore __wt_item;
%ignore __wt_lsn;
%ignore __wt_session::msg_printf;

%ignore wiredtiger_struct_pack;
%ignore wiredtiger_struct_size;
%ignore wiredtiger_struct_unpack;

%ignore wiredtiger_version;

%ignore __wt_connection::get_extension_api;
%ignore wiredtiger_extension_init;
%ignore wiredtiger_extension_terminate;

%define REQUIRE_WRAP(typedefname, name, javaname)
%ignore name;
%javamethodmodifiers name##_wrap "
  /**
   * @copydoc typedefname
   */
  public ";
%rename(javaname) name##_wrap;
%enddef

REQUIRE_WRAP(::wiredtiger_open, wiredtiger_open, open)
REQUIRE_WRAP(WT_CONNECTION::async_new_op,
    __wt_connection::async_new_op, async_new_op)
REQUIRE_WRAP(WT_CONNECTION::open_session,
    __wt_connection::open_session, open_session)
REQUIRE_WRAP(WT_SESSION::transaction_pinned_range,
    __wt_session::transaction_pinned_range, transaction_pinned_range)
REQUIRE_WRAP(WT_SESSION::open_cursor, __wt_session::open_cursor, open_cursor)
REQUIRE_WRAP(WT_ASYNC_OP::get_id, __wt_async_op::get_id,getId)

%rename(AsyncOp) __wt_async_op;
%rename(Cursor) __wt_cursor;
%rename(Session) __wt_session;
%rename(Connection) __wt_connection;

%define TRACKED_CLASS(jclassname, ctypename, java_init_fcn, implclass)
%ignore jclassname::jclassname();

%typemap(javabody) struct ctypename %{
 private long swigCPtr;
 protected boolean swigCMemOwn;

 protected $javaclassname(long cPtr, boolean cMemoryOwn) {
   swigCMemOwn = cMemoryOwn;
   swigCPtr = cPtr;
   java_init_fcn(swigCPtr, this, this);
 }

 protected static long getCPtr($javaclassname obj) {
   return (obj == null) ? 0 : obj.swigCPtr;
 }
%}

%extend ctypename {
	%javamethodmodifiers _java_init "protected";
	int _java_init(jobject jsess) {
		implclass *session = (implclass *)$self;
		JAVA_CALLBACK *jcb = (JAVA_CALLBACK *)session->lang_private;
		jcb->jobj = JCALL1(NewGlobalRef, jcb->jnienv, jsess);
		JCALL1(DeleteLocalRef, jcb->jnienv, jsess);
		return (0);
	}
}
%enddef

TRACKED_CLASS(Session, __wt_session, wiredtigerJNI.Session__java_init, WT_SESSION_IMPL)
TRACKED_CLASS(Connection, __wt_connection, wiredtigerJNI.Connection__java_init, WT_CONNECTION_IMPL)
/* Note: Cursor incorporates the elements of TRACKED_CLASS into its
 * custom constructor and %extend clause.
 */

%include "wiredtiger.h"

/* Return new connections, sessions and cursors. */
%inline {
WT_CONNECTION *wiredtiger_open_wrap(JNIEnv *jenv, const char *home, const char *config) {
	extern WT_EVENT_HANDLER javaApiEventHandler;
	WT_CONNECTION *conn = NULL;
	WT_CONNECTION_IMPL *connimpl;
	JAVA_CALLBACK *jcb;
	int ret;
	if ((ret = wiredtiger_open(home, &javaApiEventHandler, config, &conn)) != 0)
		goto err;

	connimpl = (WT_CONNECTION_IMPL *)conn;
	if ((ret = __wt_calloc_def(connimpl->default_session, 1, &jcb)) != 0)
		goto err;

	jcb->jnienv = jenv;
	connimpl->lang_private = jcb;

err:	if (ret != 0)
		throwWiredTigerException(jenv, ret);
	return conn;
}
}

%extend __wt_connection {
	WT_ASYNC_OP *async_new_op_wrap(JNIEnv *jenv, const char *uri,
	    const char *config, jobject callbackObject) {
		extern WT_ASYNC_CALLBACK javaApiAsyncHandler;
		WT_ASYNC_OP *asyncop = NULL;
		WT_CONNECTION_IMPL *connimpl;
		JAVA_CALLBACK *jcb;
		int ret;

		if ((ret = $self->async_new_op($self, uri, config, &javaApiAsyncHandler, &asyncop)) != 0)
			goto err;

		connimpl = (WT_CONNECTION_IMPL *)$self;
		if ((ret = __wt_calloc_def(connimpl->default_session, 1, &jcb)) != 0)
			goto err;

		jcb->jnienv = jenv;
		jcb->session = connimpl->default_session;
		(*jenv)->GetJavaVM(jenv, &jcb->javavm);
		jcb->jcallback = JCALL1(NewGlobalRef, jenv, callbackObject);
		JCALL1(DeleteLocalRef, jenv, callbackObject);
		asyncop->c.lang_private = jcb;
		asyncop->c.flags |= WT_CURSTD_RAW;

err:		if (ret != 0)
			throwWiredTigerException(jenv, ret);
		return asyncop;
	}
}

%extend __wt_connection {
	WT_SESSION *open_session_wrap(JNIEnv *jenv, const char *config) {
		extern WT_EVENT_HANDLER javaApiEventHandler;
		WT_SESSION *session = NULL;
		WT_SESSION_IMPL *sessionimpl;
		JAVA_CALLBACK *jcb;
		int ret;

		if ((ret = $self->open_session($self, &javaApiEventHandler, config, &session)) != 0)
			goto err;

		sessionimpl = (WT_SESSION_IMPL *)session;
		if ((ret = __wt_calloc_def(sessionimpl, 1, &jcb)) != 0)
			goto err;

		jcb->jnienv = jenv;
		sessionimpl->lang_private = jcb;

err:		if (ret != 0)
			throwWiredTigerException(jenv, ret);
		return session;
	}
}

%extend __wt_session {
	WT_CURSOR *open_cursor_wrap(JNIEnv *jenv, const char *uri, WT_CURSOR_NULLABLE *to_dup, const char *config) {
		WT_CURSOR *cursor = NULL;
		JAVA_CALLBACK *jcb;
		int ret;

		if ((ret = $self->open_cursor($self, uri, to_dup, config, &cursor)) != 0)
			goto err;

		if ((ret = __wt_calloc_def((WT_SESSION_IMPL *)cursor->session,
			    1, &jcb)) != 0)
			goto err;

		if ((cursor->flags & WT_CURSTD_RAW) != 0)
			jcb->cursor_raw = true;
		if ((cursor->flags & WT_CURSTD_DUMP_JSON) == 0)
			cursor->flags |= WT_CURSTD_RAW;

		jcb->jnienv = jenv;
		jcb->session = (WT_SESSION_IMPL *)cursor->session;
		cursor->lang_private = jcb;

err:		if (ret != 0)
			throwWiredTigerException(jenv, ret);
		return cursor;
	}
}

%extend __wt_async_op {
	long get_id_wrap(JNIEnv *jenv) {
		WT_UNUSED(jenv);
		return (self->get_id(self));
	}
}

%extend __wt_session {
	long transaction_pinned_range_wrap(JNIEnv *jenv) {
		int ret;
		uint64_t range = 0;
		ret = self->transaction_pinned_range(self, &range);
		if (ret != 0)
			throwWiredTigerException(jenv, ret);
		return range;
	}
}
