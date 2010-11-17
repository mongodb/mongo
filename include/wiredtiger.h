/* vim: set filetype=c.doxygen : */

/*! \defgroup wt WiredTiger API
 * @{
 */

#include <sys/types.h>
#include <inttypes.h>

struct WIREDTIGER_CONNECTION;	  typedef struct WIREDTIGER_CONNECTION WIREDTIGER_CONNECTION;
struct WIREDTIGER_COLUMN_INFO;	  typedef struct WIREDTIGER_COLUMN_INFO WIREDTIGER_COLUMN_INFO;
struct WIREDTIGER_CURSOR;	  typedef struct WIREDTIGER_CURSOR WIREDTIGER_CURSOR;
struct WIREDTIGER_CURSOR_FACTORY; typedef struct WIREDTIGER_CURSOR_FACTORY WIREDTIGER_CURSOR_FACTORY;
struct WIREDTIGER_INDEX_INFO;	  typedef struct WIREDTIGER_INDEX_INFO WIREDTIGER_INDEX_INFO;
struct WIREDTIGER_ITEM;		  typedef struct WIREDTIGER_ITEM WIREDTIGER_ITEM;
struct WIREDTIGER_SCHEMA;	  typedef struct WIREDTIGER_SCHEMA WIREDTIGER_SCHEMA;
struct WIREDTIGER_SESSION;	  typedef struct WIREDTIGER_SESSION WIREDTIGER_SESSION;

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
struct WIREDTIGER_ITEM {
	/*!
	 * A pointer to the location in memory of the data item.
	 *
	 * For items returned by a WIREDTIGER_CURSOR, the pointer is only valid until
	 * the next operation on that cursor.  Applications that need to keep
	 * an item across multiple cursor operations must make a copy.  WTDS
	 * never copies data into the application's buffer.
	 */
	const void *data;

	/*!
	 * The number of bytes in the data item.
	 */
	size_t size;
};

/*!
 *
 * The WIREDTIGER_CURSOR struct is the interface to a cursor.
 *
 * Cursors allow data to be searched, stepped through and updated: the
 * so-called CRUD operations (create, read, update and delete).  Data is
 * represented by WIREDTIGER_ITEM pairs.
 *
 * Thread safety: A WIREDTIGER_CURSOR handle cannot be shared between threads: it may
 * only be used within the same thread as the encapsulating WIREDTIGER_SESSION.
 */
struct WIREDTIGER_CURSOR {
	WIREDTIGER_SESSION *session;	/*!< The session handle for this cursor. */
	const char *keyfmt;
	const char *datafmt;

	/*! \name Data access
	 * @{
	 */
	/*! Get the key for the current record. */
	int __F(get_key)(WIREDTIGER_CURSOR *cursor, ...);

	/*! Get the value for the current record. */
	int __F(get_value)(WIREDTIGER_CURSOR *cursor, ...);

	/*! Set the key for the next operation. */
	int __F(set_key)(WIREDTIGER_CURSOR *cursor, ...);

	/*! Set the data for the next operation. */
	int __F(set_value)(WIREDTIGER_CURSOR *cursor, ...);
	/*! @} */


	/*! \name Cursor positioning
	 * @{
	 */
	/*! Move to the first record. */
	int __F(first)(WIREDTIGER_CURSOR *cursor);

	/*! Move to the last record. */
	int __F(last)(WIREDTIGER_CURSOR *cursor);

	/*! Move to the next record. */
	int __F(next)(WIREDTIGER_CURSOR *cursor);

	/*! Move to the previous record. */
	int __F(prev)(WIREDTIGER_CURSOR *cursor);

	/*! Search for a record. */
	int __F(search)(WIREDTIGER_CURSOR *cursor, int *exact);
	/*! @} */


	/*! \name Data modification
	 * @{
	 */
	/*! Insert a record. */
	int __F(insert)(WIREDTIGER_CURSOR *cursor);

	/*! Update the current record. */
	int __F(update)(WIREDTIGER_CURSOR *cursor);

	/*! Delete the current record. */
	int __F(del)(WIREDTIGER_CURSOR *cursor);
	/*! @} */

	/*! Close the cursor. */
	int __F(close)(WIREDTIGER_CURSOR *cursor, const char *config);
};

/*!
 * All data operations are performed in the context of a WIREDTIGER_SESSION.  This
 * encapsulates the thread and transactional context of the operation.
 *
 * Thread safety: A WIREDTIGER_SESSION handle cannot be shared between threads: it may
 * only be used within a single thread.
 */
struct WIREDTIGER_SESSION {
	/*! The connection for this session. */
	WIREDTIGER_CONNECTION *connection;

	/*! Callback to compare keys in a table. */
	int (*handle_error)(WIREDTIGER_SESSION *session, const char *err);

	/*! Close the session. */
	int __F(close)(WIREDTIGER_SESSION *session, const char *config);

	/*! \name Cursor handles
	 * @{
	 */
	/*! Open a cursor.
	 *
	 * Cursors may be opened on ordinary tables.  A cache of recently-used
	 * tables will be maintained in the WIREDTIGER_SESSION to make this fast.
	 *
	 * However, cursors can be opened on any data source, regardless of
	 * whether it is ultimately stored in a table.  Some cursor types may
	 * have limited functionality (e.g., be read-only, or not support
	 * transactional updates).
	 *
	 * The following are builtin cursor types:
	 *   <table>
	 *   <tr><th>URI</th><th>Function</th></tr>
	 *   <tr><td><tt>table:[\<tablename\>]</tt></td><td>ordinary table cursor</td></tr>
	 *   <tr><td><tt>column:[\<tablename\>.\<columnname\>]</tt></td><td>column cursor</td></tr>
	 *   <tr><td><tt>config:[table:\<tablename\>]</tt></td><td>database or table configuration</td></tr>
	 *   <tr><td><tt>cursortype:</tt></td><td>types of cursor (key=(string)prefix, data=NULL)</td></tr>
	 *   <tr><td><tt>join:\<cursor1\>\&\<cursor2\>[&\<cursor3\>...]</tt></td><td>Join the contents of multiple cursors together.</td></tr>
	 *   <tr><td><tt>module:</tt></td><td>loadable modules (key=(string)name, data=(string)path)</td></tr>
	 *   <tr><td><tt>sequence:[\<seqname\>]</tt></td><td>Sequence cursor (key=recno, data=NULL)</td></tr>
	 *   <tr><td><tt>statistics:[table:\<tablename\>]</tt></td><td>database or table statistics (key=(string)keyname, data=(int64_t)value)</td></tr>
	 *   </table>
	 *
	 * \param config a string that configures the cursor.
	 * 	For example, may include <tt>"isolation=read-uncommitted"</tt>
	 * 	and/or <tt>"nodup"</tt> and/or <tt>"overwrite"</tt> to change
	 * 	the behavior of the cursor.
	 */
	int __F(open_cursor)(WIREDTIGER_SESSION *session, const char *uri, const char *config, WIREDTIGER_CURSOR **cursorp);

	/*! Duplicate a cursor. */
	int __F(dup_cursor)(WIREDTIGER_SESSION *, WIREDTIGER_CURSOR *cursor, const char *config, WIREDTIGER_CURSOR *dupp);
	/*! @} */

	/*! \name Table operations
	 * @{
	 */
	/*! Create a table. */
	int __F(create_table)(WIREDTIGER_SESSION *session, const char *name, const char *config);

	/*! Rename a table. */
	int __F(rename_table)(WIREDTIGER_SESSION *session, const char *oldname, const char *newname);

	/*! Drop (delete) a table. */
	int __F(drop_table)(WIREDTIGER_SESSION *session, const char *name, const char *config);

	/*! Truncate a table. */
	int __F(truncate_table)(WIREDTIGER_SESSION *session, const char *name, WIREDTIGER_CURSOR *start, WIREDTIGER_CURSOR *end);

	/*! Verify a table. */
	int __F(verify_table)(WIREDTIGER_SESSION *session, const char *name, const char *config);
	/*! @} */

	/*! \name Transactions
	 * @{
	 */
	/*! Start a transaction in this session.
	 *
	 * All cursors opened in this session that support transactional
	 * semantics will operate in the context of the transaction.  The
	 * transaction remains active until ended with
	 * WIREDTIGER_SESSION::commit_transaction or WIREDTIGER_SESSION::rollback_transaction.
	 *
	 * Ignored if a transaction is in progress.
	 *
	 * \param session the session handle.
	 * \param config a configuration string (see 'CONFIGURATION' below)
	 */
	int __F(begin_transaction)(WIREDTIGER_SESSION *session, const char *config);

	/*! Commit the current transaction.
	 *
	 * Any cursors opened during the transaction will be closed before
	 * the commit is processed.
	 *
	 * Ignored if no transaction is in progress.
	 */
	int __F(commit_transaction)(WIREDTIGER_SESSION *session);

	/*! Roll back the current transaction.
	 *
	 * Any cursors opened during the transaction will be closed before
	 * the rollback is processed.
	 *
	 * Ignored if no transaction is in progress.
	 */
	int __F(rollback_transaction)(WIREDTIGER_SESSION *session);

	/*! Flush the cache and/or the log and optionally archive log files. */
	int __F(checkpoint)(WIREDTIGER_SESSION *session, const char *config);
	/*! @} */

	/*! \name Helpers for schema implementations
	 * @{
	 */
	/*! Get the thread-local cookie for the specified schema. */
	void *__F(get_schema_cookie)(WIREDTIGER_SESSION *session, const char *schema_name);
	/*! Set the thread-local cookie for the specified schema. */
	int __F(set_schema_cookie)(WIREDTIGER_SESSION *session, const char *schema_name, void *cookie);
	/*! @} */
};

/*!
 * A connection to a WTDS database.  The datastore may be opened within the
 * same address space as the caller or accessed over a socket or named pipe.
*/
struct WIREDTIGER_CONNECTION {
	const char *home;

	/*! Did opening this handle create the database? */
	int is_new;

	/*! Close a connection.
	 *
	 * Any open sessions will be closed.
	 */
	int __F(close)(WIREDTIGER_CONNECTION *connection, const char *config);

	/*! Open a session. */
	int __F(open_session)(WIREDTIGER_CONNECTION *connection, const char *config, WIREDTIGER_SESSION **sessionp);

	/*! Register a new type of cursor. */
	int __F(add_cursor_factory)(WIREDTIGER_CONNECTION *connection, const char *prefix, WIREDTIGER_CURSOR_FACTORY *factory, const char *config);

	/*! Register an extension. */
	int __F(add_extension)(WIREDTIGER_CONNECTION *connection, const char *prefix, const char *path, const char *config);

	/*! Register a new schema. */
	int __F(add_schema)(WIREDTIGER_CONNECTION *connection, const char *name, WIREDTIGER_SCHEMA *schema, const char *config);
};

/*!
 * Applications can extend WTDS by providing new implementation of the WIREDTIGER_CURSOR
 * interface.  This is done by implementing the WIREDTIGER_CURSOR_FACTORY interface, then
 * calling WIREDTIGER_CONNECTION#add_cursor_factory.
 *
 * Thread safety: WTDS may invoke methods on the WIREDTIGER_CURSOR_FACTORY interface from
 * multiple threads concurrently.  It is the responsibility of the implementation
 * to protect any shared data.
 */
struct WIREDTIGER_CURSOR_FACTORY {
	/*! Callback to determine how much space to allocate for a cursor.
	 *
	 * If the callback is NULL, no additional space is allocated in the
	 * WIREDTIGER_CURSOR implementation.
	 */
	int (*cursor_size)(WIREDTIGER_CURSOR_FACTORY *factory, const char *obj, size_t *sizep);

	/*! Callback to initialize a cursor. */
	int (*init_cursor)(WIREDTIGER_CURSOR_FACTORY *factory, WIREDTIGER_SESSION *session, const char *obj, WIREDTIGER_CURSOR *cursor);

	/*! Callback to duplicate a cursor. */
	int (*dup_cursor)(WIREDTIGER_CURSOR_FACTORY *factory, WIREDTIGER_SESSION *session, WIREDTIGER_CURSOR *old_cursor, WIREDTIGER_CURSOR *new_cursor);
};

/*!
 * Description of a column returned in WIREDTIGER_SCHEMA::column_info.
 */
struct WIREDTIGER_COLUMN_INFO {
	const char *name;	/*!< The name of the column. */
	int in_row;		/*!< Is this column stored in the row store? */

	/*! Callback to compare column keys. */
	int (*cmp)(WIREDTIGER_SESSION *session, WIREDTIGER_SCHEMA *schema, const WIREDTIGER_ITEM *key1, const WIREDTIGER_ITEM *key2);

	/*! Callback to extract one or more column keys. */
	int (*get_key)(WIREDTIGER_SESSION *session, WIREDTIGER_SCHEMA *schema, const WIREDTIGER_ITEM *key, const WIREDTIGER_ITEM *value, WIREDTIGER_ITEM *column_key, int *more);
};

/*!
 * Description of an index in WIREDTIGER_SCHEMA::index_info.
 */
struct WIREDTIGER_INDEX_INFO {
	const char *name;	/*!< The name of the index. */
	const char **columns;	/*!< The columns making up the index. */
	int num_columns;	/*!< The number of columns. */
};

/*!
 * Applications implement the WIREDTIGER_SCHEMA interface to manage tables containing structured data.
 */
struct WIREDTIGER_SCHEMA {
	const char *keyfmt;
	const char *datafmt;

	/*! Description of the columns in a table, an array of WIREDTIGER_SCHEMA::num_columns elements. */
	WIREDTIGER_COLUMN_INFO *column_info;

	/*! The number of columns in a table (zero for pure row-oriented tables). */
	int num_columns;

	/*! Description of the indices for a table, an array of WIREDTIGER_SCHEMA::num_indices elements. */
	WIREDTIGER_INDEX_INFO *index_info;

	/*! The number of indices for a table (zero for pure row-oriented tables). */
	int num_indices;

	/*! Space to allocate for this schema in every WIREDTIGER_SESSION handle */
	size_t cookie_size;

	/*! Callback to compare keys in a table. */
	int (*cmp)(WIREDTIGER_SESSION *session, WIREDTIGER_SCHEMA *schema, const WIREDTIGER_ITEM *key1, const WIREDTIGER_ITEM *key2);

	/*! Callback to compare duplicate values in a table. */
	int (*dup_cmp)(WIREDTIGER_SESSION *session, WIREDTIGER_SCHEMA *schema, const WIREDTIGER_ITEM *value1, const WIREDTIGER_ITEM *value2);
};

/*! Open a connection to a database. */
int wiredtiger_open(const char *home, const char *config, WIREDTIGER_CONNECTION **connectionp);

/*! Get information about an error as a string. */
const char *wiredtiger_strerror(int err);

/*! Get version information. */
const char *wiredtiger_version(int *majorp, int *minorp, int *patchp);

/*! Calculate the size required to pack a structure.
 *
 * Note that for variable-sized fields including variable-sized strings and
 * integers, the calculated sized merely reflects the expected sizes specified
 * in the format string itself.
 */
int wiredtiger_struct_size(const char *fmt);

/*! Pack a structure into a buffer.
 *
 * Uses format strings as specified in the Python struct module:
 *   http://docs.python.org/library/struct
 *
 * In addition, we add the following types:
 *   - 'u' (the default for simple table cursors), which packs a WIREDTIGER_ITEM, and
 *     unpacks to a WIREDTIGER_ITEM.
 *   - 'r', for record numbers (wiredtiger_recno_t).
 *   - 'S', which packs a NUL-terminated string (const char *) as
 *     "<n>s", where n=strlen(s)+1
 */
int wiredtiger_struct_pack(void *buffer, int size, const char *fmt, ...);

/*! Unpack a structure from a buffer.
 *
 * Inversion of ::wiredtiger_struct_pack.
 */
int wiredtiger_struct_unpack(const void *buffer, int size, const char *fmt, ...);

/*! Entry point to an extension, implemented by loadable modules. */
extern int wiredtiger_extension_init(WIREDTIGER_CONNECTION *connection, const char *config);

/*! @} */
