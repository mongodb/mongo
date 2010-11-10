/*! \mainpage WiredTiger Overview
 *
 * \section intro_sec Introduction
 *
 * The WiredTiger Data Store (WTDS) is a platform for extensible data
 * management.  This documentation describes the public interface used by
 * developers to construct applications.
 *
 * The <a href="examples.html">Examples Page</a> introduces the API and shows
 * how to use it to achieve a variety of tasks.
 *
 * \example ex_hello.c
 * This is an example of how to create and open a database.
 *
 * \example ex_access.c
 * Create, insert and access a simple table.
 *
 * \example ex_config.c
 * Demonstrate how to configure some properties of the database and tables.
 *
 * \example ex_stat.c
 * Shows how to access database and table statistics.
 *
 * \example ex_column.h
 * Header shared by the sources in the column example.
 *
 * \example ex_column_app.c
 * Shows how to create column-oriented data and access individual columns.
 *
 * \example ex_column_schema.c
 * Schema for the column example, can work as a loadable module.
 */

#include <sys/types.h>

struct WT_CONNECTION;
struct WT_SCHEMA;
struct WT_SESSION;

/*!
 * An item of data to be managed.  Data items have a pointer to the data and a
 * length (limited to 4GB for items stored in tables).  Records consist of a
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

typedef enum {
	WT_FIRST = -2,
	WT_PREV,
	WT_CURRENT,
	WT_NEXT,
	WT_LAST,
} WT_GET_TYPE;

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
	WT_SESSION *session;

	int get(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, WT_GET_TYPE);

	int search(WT_CURSOR *cursor, const WT_ITEM *key, const WT_ITEM *value, WT_GET_TYPE *);

	int insert(WT_CURSOR *cursor, WT_ITEM *key, const WT_ITEM *value);
	int update(WT_CURSOR *cursor, const WT_ITEM *);
	int del(WT_CURSOR *cursor);

	int close(WT_CURSOR *cursor, const char *config);
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
	int init_cursor(WT_CURSOR_FACTORY *factory, WT_SESSION *session, const char *obj, WT_CURSOR *);
	int cursor_size(WT_CURSOR_FACTORY *factory, const char *obj, size_t *);
};

/*!
 * Operations are performed in the context of a WT_SESSION.  This encapsulates
 * the thread and transactional context of the operation.
 *
 * Thread safety: A WT_SESSION handle cannot be shared between threads: it may
 * only be used within a single thread.
 */
struct WT_SESSION {
	WT_CONNECTION *connection;

	/*!
	 * Builtin cursor types:
	 *   <table>
	 *   <tr><th>URI</th><th>Function</th></tr>
	 *   <tr><td><tt>table:[\<tablename\>]</tt></td><td>ordinary table cursor</td></tr>
	 *   <tr><td><tt>tableconfig:</tt></td><td>table parameters (key=name, data=value)</td></tr>
	 *   <tr><td><tt>column:[\<tablename\>.\<columnname\></tt></td><td>column cursor</td></tr>
	 *   <tr><td><tt>config:</tt></td><td>configuration parameters (key=name, data=value)</td></tr>
	 *   <tr><td><tt>module:</tt></td><td>loadable modules (key=name, data=path)</td></tr>
	 *   <tr><td><tt>cursortype:</tt></td><td>types of cursor (key=prefix, data=NULL)</td></tr>
	 *   </table>
	 */
	int open_cursor(WT_SESSION *, const char *uri, const char *config, WT_CURSOR **);
	int begin_transaction(WT_SESSION *, const char *config);
	int commit_transaction(WT_SESSION *);
	int rollback_transaction(WT_SESSION *);

	int begin_child(WT_SESSION *);
	int commit_child(WT_SESSION *);
	int rollback_child(WT_SESSION *);
};


/*!
 * Description of a column returned by the WT_SCHEMA.get_column_info method.
 */
struct WT_COLUMN_INFO {
	const char *name;
	int (*cmp)(WT_SESSION *session, WT_SCHEMA *schema, const WT_ITEM *key1, const WT_ITEM *key2);
	int (*get_key)(WT_SESSION *session, WT_SCHEMA *schema, const WT_ITEM *key, const WT_ITEM *value, WT_ITEM *column_key, int *more);
};

/*!
 * Applications implement the WT_SCHEMA interface to manage tables with columns.
 */
struct WT_SCHEMA {
	int (*cmp)(WT_SESSION *session, WT_SCHEMA *schema, const WT_ITEM *key1, const WT_ITEM *key2);
	int (*dup_cmp)(WT_SESSION *session, WT_SCHEMA *schema, const WT_ITEM *value1, const WT_ITEM *value2);

	int num_columns;
	WT_COLUMN_INFO *column_info;
};

/*!
 * A connection to a WTDS database.  The datastore may be opened within the
 * same address space as the caller or accessed over a socket or named pipe.
*/
struct WT_CONNECTION {
	int open_session(WT_CONNECTION *connection, const char *config, WT_SESSION **sessionp);
	int close(WT_CONNECTION *connection, const char *config);

	int add_cursor_factory(WT_CONNECTION *connection, const char *prefix, WT_CURSOR_FACTORY *factory, const char *config);
	int remove_cursor_factory(WT_CONNECTION *connection, const char *prefix);

	int add_schema(WT_CONNECTION *connection, const char *name, WT_SCHEMA *schema, const char *config);
	int remove_schema(WT_CONNECTION *connection, const char *name);
};

int wt_open(const char *loc, const char *config, WT_CONNECTION **connectionp);
const char *wt_strerror(int err);

extern int wt_extension_exit(WT_CONNECTION *);
