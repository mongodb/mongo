/*-
 * Copyright (c) 2014-2019 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef __WIREDTIGER_EXT_H_
#define __WIREDTIGER_EXT_H_

#include <wiredtiger.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if !defined(SWIG)

/*!
 * @addtogroup wt_ext
 * @{
 */

/*!
 * Read-committed isolation level, returned by
 * WT_EXTENSION_API::transaction_isolation_level.
 */
#define WT_TXN_ISO_READ_COMMITTED 1
/*!
 * Read-uncommitted isolation level, returned by
 * WT_EXTENSION_API::transaction_isolation_level.
 */
#define WT_TXN_ISO_READ_UNCOMMITTED 2
/*!
 * Snapshot isolation level, returned by
 * WT_EXTENSION_API::transaction_isolation_level.
 */
#define WT_TXN_ISO_SNAPSHOT 3

typedef struct __wt_txn_notify WT_TXN_NOTIFY;
/*!
 * Snapshot isolation level, returned by
 * WT_EXTENSION_API::transaction_isolation_level.
 */
struct __wt_txn_notify {
    /*!
     * A method called when the session's current transaction is committed
     * or rolled back.
     *
     * @param notify a pointer to the event handler
     * @param session the current session handle
     * @param txnid the transaction ID
     * @param committed an integer value which is non-zero if the
     * transaction is being committed.
     */
    int (*notify)(WT_TXN_NOTIFY *notify, WT_SESSION *session, uint64_t txnid, int committed);
};

/*!
 * Table of WiredTiger extension methods.
 *
 * This structure is used to provide a set of WiredTiger methods to extension
 * modules without needing to link the modules with the WiredTiger library.
 *
 * The extension methods may be used both by modules that are linked with
 * the WiredTiger library (for example, a data source configured using the
 * WT_CONNECTION::add_data_source method), and by modules not linked with the
 * WiredTiger library (for example, a compression module configured using the
 * WT_CONNECTION::add_compressor method).
 *
 * To use these functions:
 * - include the wiredtiger_ext.h header file,
 * - declare a variable which references a WT_EXTENSION_API structure, and
 * - initialize the variable using WT_CONNECTION::get_extension_api method.
 *
 * @snippet ex_data_source.c WT_EXTENSION_API declaration
 *
 * The following code is from the sample compression module, where compression
 * extension functions are configured in the extension's entry point:
 *
 * @snippet nop_compress.c WT_COMPRESSOR initialization structure
 * @snippet nop_compress.c WT_COMPRESSOR initialization function
 */
struct __wt_extension_api {
/* !!! To maintain backwards compatibility, this structure is append-only. */
#if !defined(DOXYGEN)
    /*
     * Private fields.
     */
    WT_CONNECTION *conn; /* Enclosing connection */
#endif
    /*!
     * Insert an error message into the WiredTiger error stream.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param fmt a printf-like format specification
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION_API err_printf
     */
    int (*err_printf)(WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *fmt, ...);

    /*!
     * Insert a message into the WiredTiger message stream.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param fmt a printf-like format specification
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION_API msg_printf
     */
    int (*msg_printf)(WT_EXTENSION_API *, WT_SESSION *session, const char *fmt, ...);

    /*!
     * Return information about an error as a string.
     *
     * @snippet ex_data_source.c WT_EXTENSION_API strerror
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param error a return value from a WiredTiger function
     * @returns a string representation of the error
     */
    const char *(*strerror)(WT_EXTENSION_API *, WT_SESSION *session, int error);

    /*!
     * Map a Windows system error code to a POSIX 1003.1/ANSI C error.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param windows_error a Windows system error code
     * @returns a string representation of the error
     *
     * @snippet ex_data_source.c WT_EXTENSION_API map_windows_error
     */
    int (*map_windows_error)(WT_EXTENSION_API *wt_api, WT_SESSION *session, uint32_t windows_error);

    /*!
     * Allocate short-term use scratch memory.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param bytes the number of bytes of memory needed
     * @returns A valid memory reference on success or NULL on error
     *
     * @snippet ex_data_source.c WT_EXTENSION_API scr_alloc
     */
    void *(*scr_alloc)(WT_EXTENSION_API *wt_api, WT_SESSION *session, size_t bytes);

    /*!
     * Free short-term use scratch memory.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param ref a memory reference returned by WT_EXTENSION_API::scr_alloc
     *
     * @snippet ex_data_source.c WT_EXTENSION_API scr_free
     */
    void (*scr_free)(WT_EXTENSION_API *, WT_SESSION *session, void *ref);

    /*!
     * Configure the extension collator method.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param uri the URI of the handle being configured
     * @param config the configuration information passed to an application
     * @param collatorp the selector collator, if any
     * @param ownp set if the collator terminate method should be called
     * when no longer needed
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION collator config
     */
    int (*collator_config)(WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *uri,
      WT_CONFIG_ARG *config, WT_COLLATOR **collatorp, int *ownp);

    /*!
     * The extension collator method.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param collator the collator (or NULL if none available)
     * @param first first item
     * @param second second item
     * @param[out] cmp set less than 0 if \c first collates less than
     * \c second, set equal to 0 if \c first collates equally to \c second,
     * set greater than 0 if \c first collates greater than \c second
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION collate
     */
    int (*collate)(WT_EXTENSION_API *wt_api, WT_SESSION *session, WT_COLLATOR *collator,
      WT_ITEM *first, WT_ITEM *second, int *cmp);

    /*!
     * Return the value of a configuration key.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param config the configuration information passed to an application
     * @param key configuration key string
     * @param value the returned value
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION config_get
     */
    int (*config_get)(WT_EXTENSION_API *wt_api, WT_SESSION *session, WT_CONFIG_ARG *config,
      const char *key, WT_CONFIG_ITEM *value);

    /*!
     * Return the value of a configuration key from a string.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param config the configuration string
     * @param key configuration key string
     * @param value the returned value
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION config_get
     */
    int (*config_get_string)(WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *config,
      const char *key, WT_CONFIG_ITEM *value);

    /*!
     * @copydoc wiredtiger_config_parser_open
     */
    int (*config_parser_open)(WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *config,
      size_t len, WT_CONFIG_PARSER **config_parserp);

    /*!
     * @copydoc wiredtiger_config_parser_open
     */
    int (*config_parser_open_arg)(WT_EXTENSION_API *wt_api, WT_SESSION *session,
      WT_CONFIG_ARG *config, WT_CONFIG_PARSER **config_parserp);

    /*!
     * Insert a row into the metadata if it does not already exist.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param key row key
     * @param value row value
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION metadata insert
     */
    int (*metadata_insert)(
      WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *key, const char *value);

    /*!
     * Remove a row from the metadata.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param key row key
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION metadata remove
     */
    int (*metadata_remove)(WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *key);

    /*!
     * Return a row from the metadata.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param key row key
     * @param [out] valuep the row value
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION metadata search
     */
    int (*metadata_search)(
      WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *key, char **valuep);

    /*!
     * Update a row in the metadata by either inserting a new record or
     * updating an existing record.
     *
     * @param wt_api the extension handle
     * @param session the session handle (or NULL if none available)
     * @param key row key
     * @param value row value
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION metadata update
     */
    int (*metadata_update)(
      WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *key, const char *value);

    /*!
     * Pack a structure into a buffer. Deprecated in favor of stream
     * based pack and unpack API. See WT_EXTENSION_API::pack_start for
     * details.
     *
     * @param wt_api the extension handle
     * @param session the session handle
     * @param buffer a pointer to a packed byte array
     * @param size the number of valid bytes in the buffer
     * @param format the data format, see @ref packing
     * @errors
     */
    int (*struct_pack)(WT_EXTENSION_API *wt_api, WT_SESSION *session, void *buffer, size_t size,
      const char *format, ...);

    /*!
     * Calculate the size required to pack a structure. Deprecated in
     * favor of stream based pack and unpack API.
     *
     * @param wt_api the extension handle
     * @param session the session handle
     * @param sizep a location where the number of bytes needed for the
     * matching call to WT_EXTENSION_API::struct_pack is returned
     * @param format the data format, see @ref packing
     * @errors
     */
    int (*struct_size)(
      WT_EXTENSION_API *wt_api, WT_SESSION *session, size_t *sizep, const char *format, ...);

    /*!
     * Unpack a structure from a buffer. Deprecated in favor of stream
     * based pack and unpack API. See WT_EXTENSION_API::unpack_start for
     * details.
     *
     * @param wt_api the extension handle
     * @param session the session handle
     * @param buffer a pointer to a packed byte array
     * @param size the number of valid bytes in the buffer
     * @param format the data format, see @ref packing
     * @errors
     */
    int (*struct_unpack)(WT_EXTENSION_API *wt_api, WT_SESSION *session, const void *buffer,
      size_t size, const char *format, ...);

    /*
     * Streaming pack/unpack API.
     */
    /*!
     * Start a packing operation into a buffer.
     * See ::wiredtiger_pack_start for details.
     *
     * @param session the session handle
     * @param format the data format, see @ref packing
     * @param buffer a pointer to memory to hold the packed data
     * @param size the size of the buffer
     * @param[out] psp the new packing stream handle
     * @errors
     */
    int (*pack_start)(WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *format,
      void *buffer, size_t size, WT_PACK_STREAM **psp);

    /*!
     * Start an unpacking operation from a buffer.
     * See ::wiredtiger_unpack_start for details.
     *
     * @param session the session handle
     * @param format the data format, see @ref packing
     * @param buffer a pointer to memory holding the packed data
     * @param size the size of the buffer
     * @param[out] psp the new packing stream handle
     * @errors
     */
    int (*unpack_start)(WT_EXTENSION_API *wt_api, WT_SESSION *session, const char *format,
      const void *buffer, size_t size, WT_PACK_STREAM **psp);

    /*!
     * Close a packing stream.
     *
     * @param ps the packing stream handle
     * @param[out] usedp the number of bytes in the buffer used by the
     * stream
     * @errors
     */
    int (*pack_close)(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, size_t *usedp);

    /*!
     * Pack an item into a packing stream.
     *
     * @param ps the packing stream handle
     * @param item an item to pack
     * @errors
     */
    int (*pack_item)(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, WT_ITEM *item);

    /*!
     * Pack a signed integer into a packing stream.
     *
     * @param ps the packing stream handle
     * @param i a signed integer to pack
     * @errors
     */
    int (*pack_int)(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, int64_t i);

    /*!
     * Pack a string into a packing stream.
     *
     * @param ps the packing stream handle
     * @param s a string to pack
     * @errors
     */
    int (*pack_str)(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, const char *s);

    /*!
     * Pack an unsigned integer into a packing stream.
     *
     * @param ps the packing stream handle
     * @param u an unsigned integer to pack
     * @errors
     */
    int (*pack_uint)(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, uint64_t u);

    /*!
     * Unpack an item from a packing stream.
     *
     * @param ps the packing stream handle
     * @param item an item to unpack
     * @errors
     */
    int (*unpack_item)(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, WT_ITEM *item);

    /*!
     * Unpack a signed integer from a packing stream.
     *
     * @param ps the packing stream handle
     * @param[out] ip the unpacked signed integer
     * @errors
     */
    int (*unpack_int)(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, int64_t *ip);

    /*!
     * Unpack a string from a packing stream.
     *
     * @param ps the packing stream handle
     * @param[out] sp the unpacked string
     * @errors
     */
    int (*unpack_str)(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, const char **sp);

    /*!
     * Unpack an unsigned integer from a packing stream.
     *
     * @param ps the packing stream handle
     * @param[out] up the unpacked unsigned integer
     * @errors
     */
    int (*unpack_uint)(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, uint64_t *up);

    /*!
     * Return the current transaction ID.
     *
     * @param wt_api the extension handle
     * @param session the session handle
     * @returns the current transaction ID.
     *
     * @snippet ex_data_source.c WT_EXTENSION transaction ID
     */
    uint64_t (*transaction_id)(WT_EXTENSION_API *wt_api, WT_SESSION *session);

    /*!
     * Return the current transaction's isolation level; returns one of
     * ::WT_TXN_ISO_READ_COMMITTED, ::WT_TXN_ISO_READ_UNCOMMITTED, or
     * ::WT_TXN_ISO_SNAPSHOT.
     *
     * @param wt_api the extension handle
     * @param session the session handle
     * @returns the current transaction's isolation level.
     *
     * @snippet ex_data_source.c WT_EXTENSION transaction isolation level
     */
    int (*transaction_isolation_level)(WT_EXTENSION_API *wt_api, WT_SESSION *session);

    /*!
     * Request notification of transaction resolution by specifying a
     * function to be called when the session's current transaction is
     * either committed or rolled back.  If the transaction is being
     * committed, but the notification function returns an error, the
     * transaction will be rolled back.
     *
     * @param wt_api the extension handle
     * @param session the session handle
     * @param notify a handler for commit or rollback events
     * @errors
     *
     * @snippet ex_data_source.c WT_EXTENSION transaction notify
     */
    int (*transaction_notify)(WT_EXTENSION_API *wt_api, WT_SESSION *session, WT_TXN_NOTIFY *notify);

    /*!
     * Return the oldest transaction ID not yet visible to a running
     * transaction.
     *
     * @param wt_api the extension handle
     * @param session the session handle
     * @returns the oldest transaction ID not yet visible to a running
     * transaction.
     *
     * @snippet ex_data_source.c WT_EXTENSION transaction oldest
     */
    uint64_t (*transaction_oldest)(WT_EXTENSION_API *wt_api);

    /*!
     * Return if the current transaction can see the given transaction ID.
     *
     * @param wt_api the extension handle
     * @param session the session handle
     * @param transaction_id the transaction ID
     * @returns true (non-zero) if the transaction ID is visible to the
     * current transaction.
     *
     * @snippet ex_data_source.c WT_EXTENSION transaction visible
     */
    int (*transaction_visible)(
      WT_EXTENSION_API *wt_api, WT_SESSION *session, uint64_t transaction_id);

    /*!
     * @copydoc wiredtiger_version
     */
    const char *(*version)(int *majorp, int *minorp, int *patchp);
};

/*!
 * @typedef WT_CONFIG_ARG
 *
 * A configuration object passed to some extension interfaces.  This is an
 * opaque type: configuration values can be queried using
 * WT_EXTENSION_API::config_get
 */

/*! @} */
#endif /* SWIG */

#if defined(__cplusplus)
}
#endif
#endif /* __WIREDTIGER_EXT_H_ */
