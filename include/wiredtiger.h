/* vim: set filetype=c.doxygen : */

/*! \defgroup wt WiredTiger API
 * @{
 */

#include <sys/types.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WT_CONNECTION;	  typedef struct WT_CONNECTION WT_CONNECTION;
struct WT_CURSOR;	  typedef struct WT_CURSOR WT_CURSOR;
struct WT_CURSOR_FACTORY; typedef struct WT_CURSOR_FACTORY WT_CURSOR_FACTORY;
struct WT_ERROR_HANDLER;  typedef struct WT_ERROR_HANDLER WT_ERROR_HANDLER;
struct WT_ITEM;		  typedef struct WT_ITEM WT_ITEM;
struct WT_SCHEMA;	  typedef struct WT_SCHEMA WT_SCHEMA;
struct WT_SCHEMA_COLUMN_SET;	  typedef struct WT_SCHEMA_COLUMN_SET WT_SCHEMA_COLUMN_SET;
struct WT_SCHEMA_INDEX;	  typedef struct WT_SCHEMA_INDEX WT_SCHEMA_INDEX;
struct WT_SESSION;	  typedef struct WT_SESSION WT_SESSION;

#ifdef DOXYGEN
#define __F(func) func
#else
#define __F(func) (*func)
#endif

/*! Type of record numbers. */
typedef uint64_t wiredtiger_recno_t;

/*!
 * A raw item of data to be managed.  Data items have a pointer to the data and
 * a length (limited to 4GB for items stored in tables).  Records consist of a
 * pair of items: a key and an associated value.
 */
struct WT_ITEM {
	/*!
	 * The memory reference of the data item.
	 *
	 * For items returned by a WT_CURSOR, the pointer is only valid until
	 * the next operation on that cursor.  Applications that need to keep
	 * an item across multiple cursor operations must make a copy.
	 */
	const void *data;

	/*!
	 * The number of bytes in the data item.
	 */
	uint32_t size;
};

/*!
 *
 * The WT_CURSOR struct is the interface to a cursor.
 *
 * Cursors allow data to be searched, stepped through and updated: the
 * so-called CRUD operations (create, read, update and delete).  Data is
 * represented by WT_ITEM pairs.
 *
 * WT_CURSOR represents a cursor over a collection of data.  Cursors are opened
 * in the context of a session (which may have an associated transaction), and
 * can query and update records.  In the common case, a cursor is used to
 * access records in a table.  However, cursors can be used on subsets of
 * tables (such as a single column or a projection of multiple columns), as an
 * interface to statistics, configuration data or application-specific data
 * sources.  See WT_SESSION::open_cursor for more information.
 *
 * <b>Thread safety:</b> A WT_CURSOR handle cannot be shared between threads:
 * it may only be used within the same thread as the encapsulating WT_SESSION.
 */
struct WT_CURSOR {
	WT_SESSION *session;	/*!< The session handle for this cursor. */
	
	/*!
	 * The format of the data packed into key items.  See
	 * ::wiredtiger_struct_pack for details.  If not set, a default value
	 * of "u" is assumed, and applications use the WT_ITEM struct to
	 * manipulate raw byte arrays.
	 */
	const char *keyfmt;

	/*!
	 * The format of the data packed into value items.  See
	 * ::wiredtiger_struct_pack for details.  If not set, a default value
	 * of "u" is assumed, and applications use the WT_ITEM struct to
	 * manipulate raw byte arrays.
	 */
	const char *valuefmt;

	/*! \name Data access
	 * @{
	 */
	/*! Get the key for the current record.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(get_key)(WT_CURSOR *cursor, ...);

	/*! Get the value for the current record.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(get_value)(WT_CURSOR *cursor, ...);

	/*! Set the key for the next operation.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(set_key)(WT_CURSOR *cursor, ...);

	/*! Set the data for the next operation.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(set_value)(WT_CURSOR *cursor, ...);
	/*! @} */


	/*! \name Cursor positioning
	 * @{
	 */
	/*! Move to the first record.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(first)(WT_CURSOR *cursor);

	/*! Move to the last record.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(last)(WT_CURSOR *cursor);

	/*! Move to the next record.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(next)(WT_CURSOR *cursor);

	/*! Move to the previous record.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(prev)(WT_CURSOR *cursor);

	/*! Search for a record.
	 *
	 * \param cursor the cursor handle
	 * \param exactp the status of the search: 0 if an exact match is found, -1 if a smaller key is found, +1 if a larger key is found
	 * \errors
	 */
	int __F(search)(WT_CURSOR *cursor, int *exactp);
	/*! @} */


	/*! \name Data modification
	 * @{
	 */
	/*! Insert a record.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(insert)(WT_CURSOR *cursor);

	/*! Update the current record.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(update)(WT_CURSOR *cursor);

	/*! Delete the current record.
	 *
	 * \param cursor the cursor handle
	 * \errors
	 */
	int __F(del)(WT_CURSOR *cursor);
	/*! @} */

	/*! Close the cursor.
	 *
	 * \param cursor the cursor handle
	 * \configempty
	 * \errors
	 */
	int __F(close)(WT_CURSOR *cursor, const char *config);
};

/*!
 * The interface implemented by applications in order to handle errors.
 */
struct WT_ERROR_HANDLER {
	/*! Callback to handle errors within the session. */
	int (*handle_error)(WT_ERROR_HANDLER *handler, int err, const char *errmsg);

	/*! Optional callback to retrieve buffered messages. */
	int (*get_messages)(WT_ERROR_HANDLER *handler, const char **errmsgp);

	/*! Optional callback to clear buffered messages. */
	int (*clear_messages)(WT_ERROR_HANDLER *handler);
};

/*!
 * All data operations are performed in the context of a WT_SESSION.  This
 * encapsulates the thread and transactional context of the operation.
 *
 * <b>Thread safety:</b> A WT_SESSION handle cannot be shared between threads:
 * it may only be used within a single thread.  Each thread accessing a
 * database should open a separate WT_SESSION handle.
 */
struct WT_SESSION {
	/*! The connection for this session. */
	WT_CONNECTION *connection;

	/*! Close the session.
	 *
	 * \param session the session handle
	 * \configempty
	 * \errors
	 */
	int __F(close)(WT_SESSION *session, const char *config);

	/*! \name Cursor handles
	 * @{
	 */

	/*! Open a cursor.
	 *
	 * Cursors may be opened on ordinary tables.  A cache of recently-used
	 * tables will be maintained in the WT_SESSION to make this fast.
	 *
	 * However, cursors can be opened on any data source, regardless of
	 * whether it is ultimately stored in a table.  Some cursor types may
	 * have limited functionality (e.g., be read-only, or not support
	 * transactional updates).
	 *
	 * These are some of the common builtin cursor types:
	 *   <table>
	 *   <tr><th>URI</th><th>Function</th></tr>
	 *   <tr><td><tt>table:[\<tablename\>]</tt></td><td>ordinary table cursor</td></tr>
	 *   <tr><td><tt>column:[\<tablename\>.\<columnname\>]</tt></td><td>column cursor</td></tr>
	 *   <tr><td><tt>config:[table:\<tablename\>]</tt></td><td>database or table configuration</td></tr>
	 *   <tr><td><tt>join:\<cursor1\>\&\<cursor2\>[&\<cursor3\>...]</tt></td><td>Join the contents of multiple cursors together.</td></tr>
	 *   <tr><td><tt>statistics:[table:\<tablename\>]</tt></td><td>database or table statistics (key=(string)keyname, data=(int64_t)value)</td></tr>
	 *   </table>
	 *
	 * See \ref cursor_types for more information.
	 *
	 * \param session the session handle
	 * \param uri the data source on which the cursor operates
	 * \param session the session handle
	 * \configstart
	 * \config{dup,["all"] or "first" or "last",duplicate handling}
	 * \config{isolation,"snapshot" or ["read-committed"] or "read-uncommitted",the isolation level for this cursor.  Ignored for transactional cursors}
	 * \config{overwrite,["0"] or "1",if an existing key is inserted\, overwrite the existing value}
	 * \config{raw,["0"] or "1",ignore the encodings for the key and value\, return data as if the formats were 'u'}
	 * \configend
	 * \param cursorp a pointer to the newly opened cursor
	 * \errors
	 */
	int __F(open_cursor)(WT_SESSION *session, const char *uri, const char *config, WT_CURSOR **cursorp);

	/*! Duplicate a cursor.
	 *
	 * \param session the session handle
	 * \param cursor the cursor handle to duplicate
	 * \configstart
	 * \config{dup,["all"] or "first" or "last",duplicate handling}
	 * \config{overwrite,["0"] or "1",if an existing key is inserted\, overwrite the existing value}
	 * \config{raw,["0"] or "1",ignore the encodings for the key and value\, return data as if the formats were 'u'}
	 * \configend
	 * \param dupp a pointer to the new cursor
	 * \errors
	 */
	int __F(dup_cursor)(WT_SESSION *session, WT_CURSOR *cursor, const char *config, WT_CURSOR **dupp);
	/*! @} */

	/*! \name Table operations
	 * @{
	 */
	/*! Create a table.
	 *
	 * \param session the session handle
	 * \param name the name of the table
	 * \configstart
	 * \config{keyfmt,data format for keys ['u'],See \ref ::wiredtiger_struct_pack}
	 * \config{valuefmt,data format for values ['u'],See \ref ::wiredtiger_struct_pack}
	 * \config{schema,[none],name of a schema for the table}
	 * \configend
	 * \errors
	 */
	int __F(create_table)(WT_SESSION *session, const char *name, const char *config);

	/*! Rename a table.
	 *
	 * \param session the session handle
	 * \param oldname the current name of the table
	 * \param newname the new name of the table
	 * \configempty
	 * \errors
	 */
	int __F(rename_table)(WT_SESSION *session, const char *oldname, const char *newname, const char *config);

	/*! Drop (delete) a table.
	 *
	 * \param session the session handle
	 * \param name the name of the table
	 * \configempty
	 * \errors
	 */
	int __F(drop_table)(WT_SESSION *session, const char *name, const char *config);

	/*! Truncate a table.
	 *
	 * \param session the session handle
	 * \param name the name of the table
	 * \param start optional cursor marking the start of the truncate operation.  If <code>NULL</code>, the truncate starts from the beginning of the table
	 * \param end optional cursor marking the end of the truncate operation.  If <code>NULL</code>, the truncate continues to the end of the table
	 * \param name the name of the table
	 * \configempty
	 * \errors
	 */
	int __F(truncate_table)(WT_SESSION *session, const char *name, WT_CURSOR *start, WT_CURSOR *end, const char *config);

	/*! Verify a table.
	 *
	 * \param session the session handle
	 * \param name the name of the table
	 * \configempty
	 * \errors
	 */
	int __F(verify_table)(WT_SESSION *session, const char *name, const char *config);
	/*! @} */

	/*! \name Transactions
	 * @{
	 */
	/*! Start a transaction in this session.
	 *
	 * All cursors opened in this session that support transactional
	 * semantics will operate in the context of the transaction.  The
	 * transaction remains active until ended with
	 * WT_SESSION::commit_transaction or WT_SESSION::rollback_transaction.
	 *
	 * Ignored if a transaction is in progress.
	 *
	 * \param session the session handle
	 * \configstart
	 * \config{isolation,"serializable" or ["snapshot"] or<br>"read-committed" or "read-uncommitted",the isolation level for this transaction}
	 * \config{name,[none],name of the transaction for tracing and debugging}
	 * \config{sync,["full"] or "flush" or "write" or "none",how to sync log records when the transaction commits}
	 * \config{priority,integer between -100 and 100 ["0"],priority of the transaction for resolving conflicts}
	 * \configend
	 * \errors
	 */
	int __F(begin_transaction)(WT_SESSION *session, const char *config);

	/*! Commit the current transaction.
	 *
	 * Any cursors opened during the transaction will be closed before
	 * the commit is processed.
	 *
	 * Ignored if no transaction is in progress.
	 *
	 * \param session the session handle
	 * \errors
	 */
	int __F(commit_transaction)(WT_SESSION *session);

	/*! Roll back the current transaction.
	 *
	 * Any cursors opened during the transaction will be closed before
	 * the rollback is processed.
	 *
	 * Ignored if no transaction is in progress.
	 *
	 * \param session the session handle
	 * \errors
	 */
	int __F(rollback_transaction)(WT_SESSION *session);

	/*! Flush the cache and/or the log and optionally archive log files.
	 *
	 * \param session the session handle
	 * \configstart
	 * \config{archive,["0"] or "1",remove old log files}
	 * \config{force,["0"] or "1",write a new checkpoint even if nothing has changed since the last one}
	 * \config{flush_cache,"0" or ["1"],flush the cache}
	 * \config{flush_log,"0" or ["1"],flush the log}
	 * \config{log_size,[none],only proceed if more than the specified amount of log records have been written since the last checkpoint}
	 * \config{timeout,[none],only proceed if more than the specified number of milliseconds have elapsed since the last checkpoint}
	 * \configend
	 * \errors
	 */
	int __F(checkpoint)(WT_SESSION *session, const char *config);
	/*! @} */
};

/*!
 * A connection to a WiredTiger database.  The connection may be opened within
 * the same address space as the caller or accessed over a socket connection.
 *
 * Most applications will open a single connection to a database for each
 * process.  The first process to open a connection to a database will access
 * the database in its own address space.  Subsequent connections (if allowed)
 * will communicate with the first process over a socket connection to perform
 * their operations.
 */
struct WT_CONNECTION {
	/*! Register a new type of cursor.
	 *
	 * \param connection the connection handle
	 * \param prefix the prefix for location strings passed to WT_SESSION::open_cursor
	 * \param factory the application-supplied code to manage cursors of this type
	 * \configempty
	 * \errors
	 */
	int __F(add_cursor_factory)(WT_CONNECTION *connection, const char *prefix, WT_CURSOR_FACTORY *factory, const char *config);

	/*! Register an extension.
	 *
	 * \param connection the connection handle
	 * \param path the filename of the extension module
	 * \configstart
	 * \config{prefix,[none],a prefix for all names registered by this extension (e.g.\, to make namespaces distinct or during upgrades}
	 * \configend
	 * \errors
	 */
	int __F(add_extension)(WT_CONNECTION *connection, const char *path, const char *config);

	/*! Register a new schema.
	 *
	 * \param connection the connection handle
	 * \param name the name of the new schema
	 * \param schema the application-supplied schema information
	 * \configempty
	 * \errors
	 */
	int __F(add_schema)(WT_CONNECTION *connection, const char *name, WT_SCHEMA *schema, const char *config);

	/*! Close a connection.
	 *
	 * Any open sessions will be closed.
	 *
	 * \param connection the connection handle
	 * \configempty
	 * \errors
	 */
	int __F(close)(WT_CONNECTION *connection, const char *config);

	/*! The home directory of the connection.
	 *
	 * \param connection the connection handle
	 * \returns a pointer to a string naming the home directory
	 */
	const char *__F(get_home)(WT_CONNECTION *connection);

	/*! Did opening this handle create the database?
	 *
	 * \param connection the connection handle
	 * \returns false (zero) if the connection existed before the call to
	 *    ::wiredtiger_open, true (non-zero) if it was created by opening
	 *    this handle.
	 */
	int __F(is_new)(WT_CONNECTION *connection);

	/*! Open a session.
	 *
	 * \param connection the connection handle
	 * \param errhandler An error handler.  If <code>NULL</code>, the connection's error handler is used
	 * \configempty
	 * \param sessionp the new session handle
	 * \errors
	 */
	int __F(open_session)(WT_CONNECTION *connection, WT_ERROR_HANDLER *errhandler, const char *config, WT_SESSION **sessionp);
};

/*!
 * Applications can extend WiredTiger by providing new implementation of the
 * WT_CURSOR interface.  This is done by implementing the WT_CURSOR_FACTORY
 * interface, then calling WT_CONNECTION#add_cursor_factory.
 *
 * <b>Thread safety:</b> WiredTiger may invoke methods on the WT_CURSOR_FACTORY
 * interface from multiple threads concurrently.  It is the responsibility of
 * the implementation to protect any shared data.
 */
struct WT_CURSOR_FACTORY {
	/*! Callback to determine how much space to allocate for a cursor.
	 *
	 * If the callback is NULL, no additional space is allocated in the
	 * WT_CURSOR implementation.
	 *
	 * \errors
	 */
	int (*cursor_size)(WT_CURSOR_FACTORY *factory, const char *obj, size_t *sizep);

	/*! Callback to initialize a cursor. */
	int (*init_cursor)(WT_CURSOR_FACTORY *factory, WT_SESSION *session, const char *obj, WT_CURSOR *cursor);

	/*! Callback to duplicate a cursor.
	 *
	 * \errors
	 */
	int (*dup_cursor)(WT_CURSOR_FACTORY *factory, WT_SESSION *session, WT_CURSOR *old_cursor, WT_CURSOR *new_cursor);
};

/*!
 * Definition of a set of columns to store together for WT_SCHEMA::column_sets.
 */
struct WT_SCHEMA_COLUMN_SET {
	const char *name;	/*!< The name of the column set. */
	const char *columns;	/*!< The columns in the set, comma-separated. */

	/*! Optional callback to extract the column set value
	 *
	 * \errors
	 */
	int (*get_value)(WT_SESSION *session,
	    WT_SCHEMA *schema, WT_SCHEMA_COLUMN_SET *colset,
	    const WT_ITEM *key, const WT_ITEM *value, WT_ITEM *column_key);

	/*! Optional callback to compare column set values to order duplicates.
	 *
	 * \returns -1 if <code>value1 < value2</code>,
	 * 	     0 if <code>value1 == value2</code>,
	 * 	     1 if <code>value1 > value2</code>.
	 */
	int (*cmp)(WT_SESSION *session,
	    WT_SCHEMA *schema, WT_SCHEMA_COLUMN_SET *colset,
	    const WT_ITEM *value1, const WT_ITEM *value2);
};

/*!
 * Definition of an index in WT_SCHEMA::index_info.
 */
struct WT_SCHEMA_INDEX {
	const char *name;	/*!< The name of the index. */
	const char *columns;	/*!< The columns making up the index */

	/*! Optional callback to extract one or more column keys.
	 *
	 * \errors
	 */
	int (*get_key)(WT_SESSION *session,
	    WT_SCHEMA *schema, WT_SCHEMA_INDEX *index,
	    const WT_ITEM *key, const WT_ITEM *value,
	    WT_ITEM *index_key, int *more);

	/*! Optional callback to order index keys.
	 *
	 * \returns -1 if <code>key1 < key2</code>,
	 * 	     0 if <code>key1 == key2</code>,
	 * 	     1 if <code>key1 > key2</code>.
	 */
	int (*cmp)(WT_SESSION *session,
	    WT_SCHEMA *schema, WT_SCHEMA_INDEX *index,
	    const WT_ITEM *key1, const WT_ITEM *key2);
};

/*!
 * Applications implement the WT_SCHEMA interface to manage tables containing structured data.
 */
struct WT_SCHEMA {
	/*!
	 * The format of the data packed into key items.  See
	 * ::wiredtiger_struct_pack for details.  If not set, a default value
	 * of "u" is assumed, and applications use the WT_ITEM struct to
	 * manipulate raw byte arrays.
	 */
	const char *keyfmt;

	/*!
	 * The format of the data packed into value items.  See
	 * ::wiredtiger_struct_pack for details.  If not set, a default value
	 * of "u" is assumed, and applications use the WT_ITEM struct to
	 * manipulate raw byte arrays.
	 */
	const char *valuefmt;

	/*!
	 * Names of the columns in a table, comma separated.  The number of
	 * entries must match the total number of values in WT_SCHEMA::keyfmt
	 * and WT_SCHEMA::valuefmt.
	 */
	const char *column_names;

	/*!
	 * Description of the column sets for a table, terminated by a NULL.
	 * Each column set is stored separately, keyed by the primary key of
	 * the table.  Any column that does not appear in a column set is
	 * stored in an unnamed default column set for the table.
	 */
	WT_SCHEMA_COLUMN_SET *column_sets;

	/*!
	 * Description of the indices for a table, terminated by a NULL.
	 * Can be set to NULL for unindexed tables.
	 */
	WT_SCHEMA_INDEX *indices;
};

/*! Open a connection to a database.
 *
 * \param home The path to the database home directory
 * \param errhandler An error handler.  If <code>NULL</code>, a builtin error handler is installed that writes error messages to stderr
 * \configstart
 * \config{create,["0"] or "1",create the database if it does not exist}
 * \config{exclusive,["0"] or "1",fail if the database already exists}
 * \config{sharing,["0"] or "1",permit sharing between processes (will automatically start an RPC server for primary processes and use RPC for secondary processes)}
 * \config{cache_size,["1000000"],maximum heap memory to allocate for the cache}
 * \config{max_threads,["100"],maximum expected number of threads (including RPC client threads)}
 * \configend
 * \param connectionp A pointer to the newly opened connection handle
 * \errors
 */
int wiredtiger_open(const char *home, WT_ERROR_HANDLER *errhandler, const char *config, WT_CONNECTION **connectionp);

/*! Get information about an error as a string.
 *
 * \param err a return value from a WiredTiger call
 * \returns a string representation of the error
 */
const char *wiredtiger_strerror(int err);

/*! Get version information.
 *
 * \param majorp a location where the major version number is returned
 * \param minorp a location where the minor version number is returned
 * \param patchp a location where the patch version number is returned
 * \returns a string representation of the version
 */
const char *wiredtiger_version(int *majorp, int *minorp, int *patchp);

/*! Calculate the size required to pack a structure.
 *
 * Note that for variable-sized fields including variable-sized strings and
 * integers, the calculated sized merely reflects the expected sizes specified
 * in the format string itself.
 *
 * \param fmt the data format, see ::wiredtiger_struct_pack
 * \returns the number of bytes needed for the matching call to ::wiredtiger_struct_pack
 */
int wiredtiger_struct_size(const char *fmt, ...);

/*! Pack a structure into a buffer.
 *
 * Uses format strings as specified in the Python struct module:
 *   http://docs.python.org/library/struct
 *
 * The first character of the format string can be used to indicate the byte
 * order, size and alignment of the packed data, according to the following
 * table:
 * 
 * <table>
 * <tr><th>Character</th><th>Byte order</th><th>Size</th><th>Alignment</th></tr>
 * <tr><td><tt>\@</tt></td><td>native</td><td>native</td><td>native</td></tr>
 * <tr><td><tt>=</tt></td><td>native</td><td>standard</td><td>none</td></tr>
 * <tr><td><tt>&lt;</tt></td><td>little-endian</td><td>standard</td><td>none</td></tr>
 * <tr><td><tt>&gt;</tt></td><td>big-endian</td><td>standard</td><td>none</td></tr>
 * <tr><td><tt>!</tt></td><td>network (= big-endian)</td><td>standard</td><td>none</td></tr>
 * </table>
 *
 * If the first character is not one of these, '>' (big-endian) is assumed, in
 * part because it naturally sorts in lexicographic order.
 *
 * Format characters:
 * <table>
<tr><th>Format</th><th>C Type</th><th>Java type</th><th>Python type</th><th>Standard size</th></tr>
<tr><td>x</td><td>pad byte</td><td>N/A</td><td>N/A</td><td>1</td></tr>
<tr><td>c</td><td>char</td><td>char></td><td>string of length 1</td><td>1</td></tr>
<tr><td>b</td><td>signed char</td><td>byte</td><td>integer</td><td>1</td></tr>
<tr><td>B</td><td>unsigned char</td><td>byte</td><td>integer</td><td>1</td></tr>
<tr><td>?</td><td>_Bool</td><td>boolean</td><td>bool</td><td>1</td></tr>
<tr><td>h</td><td>short</td><td>short</td><td>integer</td><td>2</td></tr>
<tr><td>H</td><td>unsigned short</td><td>short</td><td>integer</td><td>2</td></tr>
<tr><td>i</td><td>int</td><td>int</td><td>integer</td><td>4</td></tr>
<tr><td>I</td><td>unsigned int</td><td>int</td><td>integer</td><td>4</td></tr>
<tr><td>l</td><td>long</td><td>int</td><td>integer</td><td>4</td></tr>
<tr><td>L</td><td>unsigned long</td><td>int</td><td>integer</td><td>4</td></tr>
<tr><td>q</td><td>long long</td><td>long</td><td>integer</td><td>8</td></tr>
<tr><td>Q</td><td>unsigned long long</td><td>long</td><td>integer</td><td>8</td></tr>
<tr><td>f</td><td>float</td><td>float</td><td>float</td><td>4</td></tr>
<tr><td>d</td><td>double</td><td>double</td><td>float</td><td>8</td></tr>
<tr><td>r</td><td>wt_recno_t</td><td>long</td><td>integer</td><td>8</td></tr>
<tr><td>s</td><td>char[]</td><td>String</td><td>string</td><td>fixed length</td></tr>
<tr><td>S</td><td>char[]</td><td>String</td><td>string</td><td>variable</td></tr>
<tr><td>u</td><td>WT_ITEM</td><td>byte[]</td><td>string</td><td>variable</td></tr>
 * </table>
 *
 * The <code>'S'</code> type is encoded as a C language string terminated by a NUL character.
 *
 * The <code>'u'</code> type is for raw byte arrays: if it appears at the end
 * of a format string (including in the default <code>"u"</code> format for
 * untyped tables), the size is not stored explicitly.  When <code>'u'</code>
 * appears within a format string, the size is stored as a 32-bit integer in
 * the same byte order as the rest of the format string, followed by the data.
 *
 * \section pack_examples Packing Examples
 *
 * For example, the string <code>"iSh"</code> will pack a 32-bit integer
 * followed by a NUL-terminated string, followed by a 16-bit integer.  The
 * default, big-endian encoding will be used, with no alignment.  This could
 * be used in C as follows:
 *
 * \code
 * char buf[100];
 * ret = wiredtiger_struct_pack(buf, sizeof (buf), "iSh", 42, "hello", -3);
 * \endcode
 *
 * Then later, the values can be unpacked as follows:
 *
 * \code
 * int i;
 * char *s;
 * short h;
 * ret = wiredtiger_struct_unpack(buf, sizeof (buf), "iSh", &i, &s, &h);
 * \endcode
 *
 * \param buffer a pointer to a packed byte array
 * \param size the number of valid bytes in the buffer
 * \param fmt the data format, see ::wiredtiger_struct_pack
 * \errors
 */
int wiredtiger_struct_pack(void *buffer, int size, const char *fmt, ...);

/*! Unpack a structure from a buffer.
 *
 * Reverse of ::wiredtiger_struct_pack: gets values out of a packed byte string.
 *
 * \param buffer a pointer to a packed byte array
 * \param size the number of valid bytes in the buffer
 * \param fmt the data format, see ::wiredtiger_struct_pack
 * \errors
 */
int wiredtiger_struct_unpack(const void *buffer, int size, const char *fmt, ...);

/*! Entry point to an extension, implemented by loadable modules.
 *
 * \param connection the connection handle
 * \configempty
 * \errors
 */
extern int wiredtiger_extension_init(WT_CONNECTION *connection, const char *config);

/*! No matching record was found, including when reaching the limits of a cursor traversal. */
#define	WT_NOTFOUND	(-10000)

/*! @} */

#ifdef __cplusplus
}
#endif
