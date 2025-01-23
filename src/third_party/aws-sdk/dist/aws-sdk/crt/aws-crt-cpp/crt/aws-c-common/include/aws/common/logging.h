#ifndef AWS_COMMON_LOGGING_H
#define AWS_COMMON_LOGGING_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/atomics.h>
#include <aws/common/common.h>
#include <aws/common/thread.h>

AWS_PUSH_SANE_WARNING_LEVEL

#define AWS_LOG_LEVEL_NONE 0
#define AWS_LOG_LEVEL_FATAL 1
#define AWS_LOG_LEVEL_ERROR 2
#define AWS_LOG_LEVEL_WARN 3
#define AWS_LOG_LEVEL_INFO 4
#define AWS_LOG_LEVEL_DEBUG 5
#define AWS_LOG_LEVEL_TRACE 6

/**
 * Controls what log calls pass through the logger and what log calls get filtered out.
 * If a log level has a value of X, then all log calls using a level <= X will appear, while
 * those using a value > X will not occur.
 *
 * You can filter both dynamically (by setting the log level on the logger object) or statically
 * (by defining AWS_STATIC_LOG_LEVEL to be an appropriate integer module-wide).  Statically filtered
 * log calls will be completely compiled out but require a rebuild if you want to get more detail
 * about what's happening.
 */
enum aws_log_level {
    AWS_LL_NONE = AWS_LOG_LEVEL_NONE,
    AWS_LL_FATAL = AWS_LOG_LEVEL_FATAL,
    AWS_LL_ERROR = AWS_LOG_LEVEL_ERROR,
    AWS_LL_WARN = AWS_LOG_LEVEL_WARN,
    AWS_LL_INFO = AWS_LOG_LEVEL_INFO,
    AWS_LL_DEBUG = AWS_LOG_LEVEL_DEBUG,
    AWS_LL_TRACE = AWS_LOG_LEVEL_TRACE,

    AWS_LL_COUNT
};

/**
 * Log subject is a way of designating the topic of logging.
 *
 * The general idea is to support a finer-grained approach to log level control.  The primary use case
 * is for situations that require more detailed logging within a specific domain, where enabling that detail
 * globally leads to an untenable flood of information.
 *
 * For example, enable TRACE logging for tls-related log statements (handshake binary payloads), but
 * only WARN logging everywhere else (because http payloads would blow up the log files).
 *
 * Log subject is an enum similar to aws error: each library has its own value-space and someone is
 * responsible for registering the value <-> string connections.
 */
typedef uint32_t aws_log_subject_t;

/* Each library gets space for 2^^10 log subject entries */
enum {
    AWS_LOG_SUBJECT_STRIDE_BITS = 10,
};
#define AWS_LOG_SUBJECT_STRIDE (1U << AWS_LOG_SUBJECT_STRIDE_BITS)
#define AWS_LOG_SUBJECT_BEGIN_RANGE(x) ((x) * AWS_LOG_SUBJECT_STRIDE)
#define AWS_LOG_SUBJECT_END_RANGE(x) (((x) + 1) * AWS_LOG_SUBJECT_STRIDE - 1)

struct aws_log_subject_info {
    aws_log_subject_t subject_id;
    const char *subject_name;
    const char *subject_description;
};

#define DEFINE_LOG_SUBJECT_INFO(id, name, desc)                                                                        \
    {.subject_id = (id), .subject_name = (name), .subject_description = (desc)}

struct aws_log_subject_info_list {
    struct aws_log_subject_info *subject_list;
    size_t count;
};

enum aws_common_log_subject {
    AWS_LS_COMMON_GENERAL = AWS_LOG_SUBJECT_BEGIN_RANGE(AWS_C_COMMON_PACKAGE_ID),
    AWS_LS_COMMON_TASK_SCHEDULER,
    AWS_LS_COMMON_THREAD,
    AWS_LS_COMMON_MEMTRACE,
    AWS_LS_COMMON_XML_PARSER,
    AWS_LS_COMMON_IO,
    AWS_LS_COMMON_BUS,
    AWS_LS_COMMON_TEST,
    AWS_LS_COMMON_JSON_PARSER,
    AWS_LS_COMMON_CBOR,

    AWS_LS_COMMON_LAST = AWS_LOG_SUBJECT_END_RANGE(AWS_C_COMMON_PACKAGE_ID)
};

struct aws_logger;
struct aws_log_formatter;
struct aws_log_channel;
struct aws_log_writer;

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4623) /* default constructor was implicitly defined as deleted */
#    pragma warning(disable : 4626) /* assignment operator was implicitly defined as deleted */
#    pragma warning(disable : 5027) /* move assignment operator was implicitly defined as deleted */
#endif

/**
 * We separate the log level function from the log call itself so that we can do the filter check in the macros (see
 * below)
 *
 * By doing so, we make it so that the variadic format arguments are not even evaluated if the filter check does not
 * succeed.
 */
struct aws_logger_vtable {
    int (*const log)(
        struct aws_logger *logger,
        enum aws_log_level log_level,
        aws_log_subject_t subject,
        const char *format,
        ...)
#if defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
        __attribute__((format(printf, 4, 5)))
#endif /* non-ms compilers: TODO - find out what versions format support was added in */
        ;
    enum aws_log_level (*const get_log_level)(struct aws_logger *logger, aws_log_subject_t subject);
    void (*const clean_up)(struct aws_logger *logger);
    int (*set_log_level)(struct aws_logger *logger, enum aws_log_level);
};

#ifdef _MSC_VER
#    pragma warning(pop)
#endif

struct aws_logger {
    struct aws_logger_vtable *vtable;
    struct aws_allocator *allocator;
    void *p_impl;
};

/**
 * The base formatted logging macro that all other formatted logging macros resolve to.
 * Checks for a logger and filters based on log level.
 */
#define AWS_LOGF(log_level, subject, ...)                                                                              \
    do {                                                                                                               \
        AWS_ASSERT(log_level > 0);                                                                                     \
        struct aws_logger *logger = aws_logger_get();                                                                  \
        if (logger != NULL && logger->vtable->get_log_level(logger, (subject)) >= (log_level)) {                       \
            logger->vtable->log(logger, log_level, subject, __VA_ARGS__);                                              \
        }                                                                                                              \
    } while (0)
/**
 * Unconditional logging macro that takes a logger and does not do a level check or a null check.  Intended for
 * situations when you need to log many things and do a single manual level check before beginning.
 */
#define AWS_LOGUF(logger, log_level, subject, ...)                                                                     \
    { logger->vtable->log(logger, log_level, subject, __VA_ARGS__); }

/**
 * LOGF_<level> variants for each level.  These are what should be used directly to do all logging.
 *
 * i.e.
 *
 * LOGF_FATAL("Device \"%s\" not found", device->name);
 *
 *
 * Later we will likely expose Subject-aware variants
 */
#if !defined(AWS_STATIC_LOG_LEVEL) || (AWS_STATIC_LOG_LEVEL >= AWS_LOG_LEVEL_FATAL)
#    define AWS_LOGF_FATAL(subject, ...) AWS_LOGF(AWS_LL_FATAL, subject, __VA_ARGS__)
#else
#    define AWS_LOGF_FATAL(subject, ...)
#endif

#if !defined(AWS_STATIC_LOG_LEVEL) || (AWS_STATIC_LOG_LEVEL >= AWS_LOG_LEVEL_ERROR)
#    define AWS_LOGF_ERROR(subject, ...) AWS_LOGF(AWS_LL_ERROR, subject, __VA_ARGS__)
#else
#    define AWS_LOGF_ERROR(subject, ...)
#endif

#if !defined(AWS_STATIC_LOG_LEVEL) || (AWS_STATIC_LOG_LEVEL >= AWS_LOG_LEVEL_WARN)
#    define AWS_LOGF_WARN(subject, ...) AWS_LOGF(AWS_LL_WARN, subject, __VA_ARGS__)
#else
#    define AWS_LOGF_WARN(subject, ...)
#endif

#if !defined(AWS_STATIC_LOG_LEVEL) || (AWS_STATIC_LOG_LEVEL >= AWS_LOG_LEVEL_INFO)
#    define AWS_LOGF_INFO(subject, ...) AWS_LOGF(AWS_LL_INFO, subject, __VA_ARGS__)
#else
#    define AWS_LOGF_INFO(subject, ...)
#endif

#if !defined(AWS_STATIC_LOG_LEVEL) || (AWS_STATIC_LOG_LEVEL >= AWS_LOG_LEVEL_DEBUG)
#    define AWS_LOGF_DEBUG(subject, ...) AWS_LOGF(AWS_LL_DEBUG, subject, __VA_ARGS__)
#else
#    define AWS_LOGF_DEBUG(subject, ...)
#endif

#if !defined(AWS_STATIC_LOG_LEVEL) || (AWS_STATIC_LOG_LEVEL >= AWS_LOG_LEVEL_TRACE)
#    define AWS_LOGF_TRACE(subject, ...) AWS_LOGF(AWS_LL_TRACE, subject, __VA_ARGS__)
#else
#    define AWS_LOGF_TRACE(subject, ...)
#endif

/*
 * Standard logger implementation composing three sub-components:
 *
 * The formatter takes var args input from the user and produces a formatted log line
 * The writer takes a formatted log line and outputs it somewhere
 * The channel is the transport between the two
 */
struct aws_logger_pipeline {
    struct aws_log_formatter *formatter;
    struct aws_log_channel *channel;
    struct aws_log_writer *writer;
    struct aws_allocator *allocator;
    struct aws_atomic_var level;
};

/**
 * Options for aws_logger_init_standard().
 * Set `filename` to open a file for logging and close it when the logger cleans up.
 * Set `file` to use a file that is already open, such as `stderr` or `stdout`.
 */
struct aws_logger_standard_options {
    enum aws_log_level level;
    const char *filename;
    FILE *file;
};

AWS_EXTERN_C_BEGIN

/**
 * Sets the aws logger used globally across the process.  Not thread-safe.  Must only be called once.
 */
AWS_COMMON_API
void aws_logger_set(struct aws_logger *logger);

/**
 * Gets the aws logger used globally across the process.
 */
AWS_COMMON_API
struct aws_logger *aws_logger_get(void);

/**
 * Gets the aws logger used globally across the process if the logging level is at least the inputted level.
 *
 * @param subject log subject to perform the level check versus, not currently used
 * @param level logging level to check against in order to return the logger
 * @return the current logger if the current logging level is at or more detailed then the supplied logging level
 */
AWS_COMMON_API
struct aws_logger *aws_logger_get_conditional(aws_log_subject_t subject, enum aws_log_level level);

/**
 * Cleans up all resources used by the logger; simply invokes the clean_up v-function
 */
AWS_COMMON_API
void aws_logger_clean_up(struct aws_logger *logger);

/**
 * Sets the current logging level for the logger.  Loggers are not require to support this.
 * @param logger logger to set the log level for
 * @param level new log level for the logger
 * @return AWS_OP_SUCCESS if the level was successfully set, AWS_OP_ERR otherwise
 */
AWS_COMMON_API
int aws_logger_set_log_level(struct aws_logger *logger, enum aws_log_level level);

/**
 * Converts a log level to a c-string constant.  Intended primarily to support building log lines that
 * include the level in them, i.e.
 *
 * [ERROR] 10:34:54.642 01-31-19 - Json parse error....
 */
AWS_COMMON_API
int aws_log_level_to_string(enum aws_log_level log_level, const char **level_string);

/**
 * Converts a c-string constant to a log level value.  Uses case-insensitive comparison
 * and simply iterates all possibilities until a match or nothing remains.  If no match
 * is found, AWS_OP_ERR is returned.
 */
AWS_COMMON_API
int aws_string_to_log_level(const char *level_string, enum aws_log_level *log_level);

/**
 * Converts an aws_thread_id_t to a c-string.  For portability, aws_thread_id_t
 * must not be printed directly.  Intended primarily to support building log
 * lines that include the thread id in them.  The parameter `buffer` must
 * point-to a char buffer of length `bufsz == AWS_THREAD_ID_T_REPR_BUFSZ`.  The
 * thread id representation is returned in `buffer`.
 */
AWS_COMMON_API
int aws_thread_id_t_to_string(aws_thread_id_t thread_id, char *buffer, size_t bufsz);

/**
 * Get subject name from log subject.
 */
AWS_COMMON_API
const char *aws_log_subject_name(aws_log_subject_t subject);

/**
 * Connects log subject strings with log subject integer values
 */
AWS_COMMON_API
void aws_register_log_subject_info_list(struct aws_log_subject_info_list *log_subject_list);

/**
 * Disconnects log subject strings with log subject integer values
 */
AWS_COMMON_API
void aws_unregister_log_subject_info_list(struct aws_log_subject_info_list *log_subject_list);

/*
 * Initializes a pipeline logger that is built from the default formatter, a background thread-based channel, and
 * a file writer.  The default logger in almost all circumstances.
 */
AWS_COMMON_API
int aws_logger_init_standard(
    struct aws_logger *logger,
    struct aws_allocator *allocator,
    struct aws_logger_standard_options *options);

/*
 * Initializes a pipeline logger from components that have already been initialized.  This is not an ownership transfer.
 * After the pipeline logger is cleaned up, the components will have to manually be cleaned up by the user.
 */
AWS_COMMON_API
int aws_logger_init_from_external(
    struct aws_logger *logger,
    struct aws_allocator *allocator,
    struct aws_log_formatter *formatter,
    struct aws_log_channel *channel,
    struct aws_log_writer *writer,
    enum aws_log_level level);

/*
 * Pipeline logger vtable for custom configurations
 */
AWS_COMMON_API
extern struct aws_logger_vtable g_pipeline_logger_owned_vtable;

/*
 * Initializes a logger that does not perform any allocation during logging.  Log lines larger than the internal
 * constant are truncated.  Formatting matches the standard logger.  Used for memory tracing logging.
 * If no file or filename is set in the aws_logger_standard_options, then it will use stderr.
 */
AWS_COMMON_API
int aws_logger_init_noalloc(
    struct aws_logger *logger,
    struct aws_allocator *allocator,
    struct aws_logger_standard_options *options);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_LOGGING_H */
