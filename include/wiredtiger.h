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
struct WT_COLUMN_INFO;	  typedef struct WT_COLUMN_INFO WT_COLUMN_INFO;
struct WT_CURSOR;	  typedef struct WT_CURSOR WT_CURSOR;
struct WT_CURSOR_FACTORY; typedef struct WT_CURSOR_FACTORY WT_CURSOR_FACTORY;
struct WT_INDEX_INFO;	  typedef struct WT_INDEX_INFO WT_INDEX_INFO;
struct WT_ITEM;		  typedef struct WT_ITEM WT_ITEM;
struct WT_SCHEMA;	  typedef struct WT_SCHEMA WT_SCHEMA;
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
	 * A pointer to the location in memory of the data item.
	 *
	 * For items returned by a WT_CURSOR, the pointer is only valid until
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
 * The WT_CURSOR struct is the interface to a cursor.
 *
 * Cursors allow data to be searched, stepped through and updated: the
 * so-called CRUD operations (create, read, update and delete).  Data is
 * represented by WT_ITEM pairs.
 *
 * Thread safety: A WT_CURSOR handle cannot be shared between threads: it may
 * only be used within the same thread as the encapsulating WT_SESSION.
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
	/*! Get the key for the current record. */
	int __F(get_key)(WT_CURSOR *cursor, ...);

	/*! Get the value for the current record. */
	int __F(get_value)(WT_CURSOR *cursor, ...);

	/*! Set the key for the next operation. */
	int __F(set_key)(WT_CURSOR *cursor, ...);

	/*! Set the data for the next operation. */
	int __F(set_value)(WT_CURSOR *cursor, ...);
	/*! @} */


	/*! \name Cursor positioning
	 * @{
	 */
	/*! Move to the first record. */
	int __F(first)(WT_CURSOR *cursor);

	/*! Move to the last record. */
	int __F(last)(WT_CURSOR *cursor);

	/*! Move to the next record. */
	int __F(next)(WT_CURSOR *cursor);

	/*! Move to the previous record. */
	int __F(prev)(WT_CURSOR *cursor);

	/*! Search for a record. */
	int __F(search)(WT_CURSOR *cursor, int *exact);
	/*! @} */


	/*! \name Data modification
	 * @{
	 */
	/*! Insert a record. */
	int __F(insert)(WT_CURSOR *cursor);

	/*! Update the current record. */
	int __F(update)(WT_CURSOR *cursor);

	/*! Delete the current record. */
	int __F(del)(WT_CURSOR *cursor);
	/*! @} */

	/*! Close the cursor. */
	int __F(close)(WT_CURSOR *cursor, const char *config);
};

/*!
 * All data operations are performed in the context of a WT_SESSION.  This
 * encapsulates the thread and transactional context of the operation.
 *
 * Thread safety: A WT_SESSION handle cannot be shared between threads: it may
 * only be used within a single thread.
 */
struct WT_SESSION {
	/*! The connection for this session. */
	WT_CONNECTION *connection;

	/*! Callback to handle errors within the session. */
	int (*handle_error)(WT_SESSION *session, const char *err);

	/*! Close the session. */
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
	 * \param session the session handle.
	 * \param uri the data source on which the cursor operates.
	 * \param config a string that configures the cursor.
	 * 	For example, may include <tt>"isolation=read-uncommitted"</tt>
	 * 	and/or <tt>"nodup"</tt> and/or <tt>"overwrite"</tt> to change
	 * 	the behavior of the cursor.
	 * \param cursorp a pointer to the newly opened cursor.
	 */
	int __F(open_cursor)(WT_SESSION *session, const char *uri, const char *config, WT_CURSOR **cursorp);

	/*! Duplicate a cursor. */
	int __F(dup_cursor)(WT_SESSION *, WT_CURSOR *cursor, const char *config, WT_CURSOR **dupp);
	/*! @} */

	/*! \name Table operations
	 * @{
	 */
	/*! Create a table. */
	int __F(create_table)(WT_SESSION *session, const char *name, const char *config);

	/*! Rename a table. */
	int __F(rename_table)(WT_SESSION *session, const char *oldname, const char *newname, const char *config);

	/*! Drop (delete) a table. */
	int __F(drop_table)(WT_SESSION *session, const char *name, const char *config);

	/*! Truncate a table. */
	int __F(truncate_table)(WT_SESSION *session, const char *name, WT_CURSOR *start, WT_CURSOR *end, const char *config);

	/*! Verify a table. */
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
	 * \param session the session handle.
	 * \param config a configuration string (see 'CONFIGURATION' below)
	 */
	int __F(begin_transaction)(WT_SESSION *session, const char *config);

	/*! Commit the current transaction.
	 *
	 * Any cursors opened during the transaction will be closed before
	 * the commit is processed.
	 *
	 * Ignored if no transaction is in progress.
	 */
	int __F(commit_transaction)(WT_SESSION *session);

	/*! Roll back the current transaction.
	 *
	 * Any cursors opened during the transaction will be closed before
	 * the rollback is processed.
	 *
	 * Ignored if no transaction is in progress.
	 */
	int __F(rollback_transaction)(WT_SESSION *session);

	/*! Flush the cache and/or the log and optionally archive log files. */
	int __F(checkpoint)(WT_SESSION *session, const char *config);
	/*! @} */

	/*! \name Helpers for schema implementations
	 * @{
	 */
	/*! Get the thread-local cookie for the specified schema. */
	void *__F(get_schema_cookie)(WT_SESSION *session, const char *schema_name);
	/*! Set the thread-local cookie for the specified schema. */
	int __F(set_schema_cookie)(WT_SESSION *session, const char *schema_name, void *cookie);
	/*! @} */
};

/*!
 * A connection to a WTDS database.  The datastore may be opened within the
 * same address space as the caller or accessed over a socket or named pipe.
*/
struct WT_CONNECTION {
	/*! The home directory of the connection. */
	const char *home;

	/*! Did opening this handle create the database? */
	int is_new;

	/*! Close a connection.
	 *
	 * Any open sessions will be closed.
	 */
	int __F(close)(WT_CONNECTION *connection, const char *config);

	/*! Open a session. */
	int __F(open_session)(WT_CONNECTION *connection, const char *config, WT_SESSION **sessionp);

	/*! Register a new type of cursor. */
	int __F(add_cursor_factory)(WT_CONNECTION *connection, const char *prefix, WT_CURSOR_FACTORY *factory, const char *config);

	/*! Register an extension. */
	int __F(add_extension)(WT_CONNECTION *connection, const char *prefix, const char *path, const char *config);

	/*! Register a new schema. */
	int __F(add_schema)(WT_CONNECTION *connection, const char *name, WT_SCHEMA *schema, const char *config);
};

/*!
 * Applications can extend WTDS by providing new implementation of the WT_CURSOR
 * interface.  This is done by implementing the WT_CURSOR_FACTORY interface, then
 * calling WT_CONNECTION#add_cursor_factory.
 *
 * Thread safety: WTDS may invoke methods on the WT_CURSOR_FACTORY interface from
 * multiple threads concurrently.  It is the responsibility of the implementation
 * to protect any shared data.
 */
struct WT_CURSOR_FACTORY {
	/*! Callback to determine how much space to allocate for a cursor.
	 *
	 * If the callback is NULL, no additional space is allocated in the
	 * WT_CURSOR implementation.
	 */
	int (*cursor_size)(WT_CURSOR_FACTORY *factory, const char *obj, size_t *sizep);

	/*! Callback to initialize a cursor. */
	int (*init_cursor)(WT_CURSOR_FACTORY *factory, WT_SESSION *session, const char *obj, WT_CURSOR *cursor);

	/*! Callback to duplicate a cursor. */
	int (*dup_cursor)(WT_CURSOR_FACTORY *factory, WT_SESSION *session, WT_CURSOR *old_cursor, WT_CURSOR *new_cursor);
};

/*!
 * Description of a column returned in WT_SCHEMA::column_info.
 */
struct WT_COLUMN_INFO {
	const char *name;	/*!< The name of the column. */
	int in_column;		/*!< Is this column stored in the row store? */

	/*! Callback to compare column keys. */
	int (*cmp)(WT_SESSION *session, WT_SCHEMA *schema, const WT_ITEM *key1, const WT_ITEM *key2);

	/*! Callback to extract one or more column keys. */
	int (*get_key)(WT_SESSION *session, WT_SCHEMA *schema, const WT_ITEM *key, const WT_ITEM *value, WT_ITEM *column_key, int *more);
};

/*!
 * Description of an index in WT_SCHEMA::index_info.
 */
struct WT_INDEX_INFO {
	const char *name;	/*!< The name of the index. */
	const char **columns;	/*!< The columns making up the index. */
	int num_columns;	/*!< The number of columns. */
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
	 * Description of the columns in a table, an array of
	 * WT_SCHEMA::num_columns elements.
	 */
	WT_COLUMN_INFO *column_info;

	/*!
	 * The number of columns in a table. The WT_SCHEMA::column_info field
	 * must be an array of this length.  This must also match the total
	 * number of values in WT_SCHEMA::keyfmt and WT_SCHEMA::valuefmt, if
	 * they are set.
	 */
	int num_columns;

	/*!
	 * Description of the indices for a table, an array of
	 * WT_SCHEMA::num_indices elements.  Set to NULL for unindexed tables.
	 */
	WT_INDEX_INFO *index_info;

	/*! The number of indices for a table (zero for unindexed tables). */
	int num_indices;

	/*! Space to allocate for this schema in every WT_SESSION handle */
	size_t cookie_size;
};

/*! Open a connection to a database. */
int wiredtiger_open(const char *home, const char *config, WT_CONNECTION **connectionp);

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
<tr><td>s</td><td>char[]</td><td>String</td><td>string</td><td>fixed length</td></tr>
<tr><td>S</td><td>char[]</td><td>String</td><td>string</td><td>N/A</td></tr>
<tr><td>r</td><td>wt_recno_t</td><td>long</td><td>integer</td><td>8</td></tr>
<tr><td>u</td><td>WT_ITEM</td><td>byte[]</td><td>string</td><td>N/A</td></tr>
 * </table>
 *
 * In addition, we add the following types:
 *   - 'u' (the default for simple table cursors), which packs a WT_ITEM, and
 *     unpacks to a WT_ITEM.
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
extern int wiredtiger_extension_init(WT_CONNECTION *connection, const char *config);

#define	WT_NOTFOUND	(-10000)

/*! @} */

#ifdef __cplusplus
}
#endif
