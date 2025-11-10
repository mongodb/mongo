/*-
 * Public Domain 2014-present MongoDB, Inc.
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
 */

#include "wiredtiger.h"
#include "wiredtiger_ext.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <ranges>
#include <shared_mutex>
#include <span>
#include <source_location>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>

using namespace std::chrono_literals;

namespace {
constexpr size_t BYTES_PER_LINE = 16;
constexpr size_t LINE_BUFFER_SIZE = 80;
constexpr size_t BUFFER_SIZE = 1024;

/* Helper to write hex bytes for one line */
char *
write_hex_bytes(char *buf_ptr, std::span<const uint8_t> line_data)
{
    for (size_t j = 0; j < BYTES_PER_LINE; ++j) {
        if (j < line_data.size()) {
            buf_ptr = std::format_to(buf_ptr, "{:02x} ", line_data[j]);
        } else {
            buf_ptr = std::format_to(buf_ptr, "   ");
        }
        if (j % 8 == 7) {
            *buf_ptr++ = ' ';
        }
    }
    return buf_ptr;
}

/* Helper to write ASCII representation for one line */
char *
write_ascii_chars(char *buf_ptr, std::span<const uint8_t> line_data)
{
    for (uint8_t c : line_data) {
        *buf_ptr++ = isascii(c) && std::isprint(c) ? c : '.';
    }
    return buf_ptr;
}

/* Helper to check if there's enough buffer space and handle truncation */
bool
check_buffer_space(char *&buf_ptr, char *buffer_start, size_t current_offset, size_t total_size)
{
    size_t used_space = buf_ptr - buffer_start;
    if (used_space + LINE_BUFFER_SIZE > BUFFER_SIZE) {
        size_t remaining_space = BUFFER_SIZE - used_space;
        auto result = std::format_to_n(
          buf_ptr, remaining_space, "... {} of {} bytes printed ...\n", current_offset, total_size);
        buf_ptr = result.out;
        return false;
    }
    return true;
}

const char *
hexdump(const void *data, size_t size)
{
    thread_local char buffer[BUFFER_SIZE];
    const auto *p = static_cast<const uint8_t *>(data);
    char *buf_ptr = buffer;
    std::span<const uint8_t> all_data(p, size);

    for (size_t i = 0; i < size; i += BYTES_PER_LINE) {
        if (!check_buffer_space(buf_ptr, buffer, i, size)) {
            break;
        }

        /* Write offset */
        buf_ptr = std::format_to(buf_ptr, "{:08x}  ", i);

        size_t chunk_size = std::min(BYTES_PER_LINE, size - i);
        std::span<const uint8_t> line_data = all_data.subspan(i, chunk_size);

        /* Write hex bytes */
        buf_ptr = write_hex_bytes(buf_ptr, line_data);

        /* Write ASCII representation */
        buf_ptr = std::format_to(buf_ptr, "|");
        buf_ptr = write_ascii_chars(buf_ptr, line_data);
        buf_ptr = std::format_to(buf_ptr, "|\n");
    }

    /* Write final offset if needed */
    if (buf_ptr < buffer + BUFFER_SIZE && (size % BYTES_PER_LINE != 0 || size == 0)) {
        buf_ptr = std::format_to(buf_ptr, "{:08x}\n", size);
    }

    /* Null-terminate safely */
    if (buf_ptr < buffer + BUFFER_SIZE) {
        *buf_ptr = '\0';
    } else {
        buffer[BUFFER_SIZE - 1] = '\0';
    }

    return buffer;
}
} /* namespace */

static const char *
verbose_item(const WT_ITEM *buf)
{
    return hexdump(buf->data, buf->size);
}

static void
resize_item(WT_ITEM *item, size_t new_size)
{
    if (item->memsize < new_size) {
        item->mem = realloc(item->mem, new_size);
        if (item->mem == NULL)
            throw std::bad_alloc();
        item->memsize = new_size;
    }
    item->data = item->mem;
    item->size = new_size;
}

static void
fill_item(WT_ITEM *item, const void *data, size_t size)
{
    memset(item, 0, sizeof(WT_ITEM));
    resize_item(item, size);
    if (size > 0 && data != nullptr)
        memcpy(item->mem, data, size);
}

/*
 * Compute a random jitter (milliseconds) around an average, uniformly in [0.5 * avg_ms, 1.5 *
 * avg_ms].
 */
static uint64_t
jitter_ms(uint32_t avg_ms)
{
    thread_local static std::mt19937_64 rng;
    uint64_t min_ms = avg_ms / 2;          /* 0.5 * avg */
    uint64_t max_ms = avg_ms + avg_ms / 2; /* 1.5 * avg */
    std::uniform_int_distribution<uint64_t> dist(min_ms, max_ms);
    return dist(rng);
}

/* Simple joiner for collections */
template <typename Range, typename Separator>
std::string
join(const Range &range, const Separator &sep)
{
    std::stringstream ss;
    auto it = std::begin(range);
    auto end = std::end(range);

    if (it != end) {
        ss << *it; /* Add the first element */
        ++it;
    }

    while (it != end) {
        ss << sep; /* Add separator before the next element */
        ss << *it; /* Add the element */
        ++it;
    }

    return ss.str();
}

/* Base-2 (binary) units */
constexpr uint64_t operator""_KB(unsigned long long val)
{
    return val * 1024;
}
constexpr uint64_t operator""_MB(unsigned long long val)
{
    return val * 1024 * 1024;
}
constexpr uint64_t operator""_GB(unsigned long long val)
{
    return val * 1024 * 1024 * 1024;
}

/* Unix epoch microseconds */
inline auto
now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

/* Configuration settings with defaults */
struct Config {
    WT_EXTENSION_API *extapi = nullptr; /* WiredTiger extension API */

    std::filesystem::path home_dir;        /* Home directory for the extension */
    uint32_t cache_size_mb = 1'024;        /* Size of cache in megabytes (default) */
    uint32_t mmap_size_mb = 1'024;         /* Size of memory map in megabytes (default) */
    uint32_t delay_ms = 0;                 /* Average length of delay when simulated */
    uint32_t error_ms = 0;                 /* Average length of sleep when simulated */
    uint32_t force_delay = 0;              /* Force a simulated network delay every N operations */
    uint32_t force_error = 0;              /* Force a simulated network error every N operations */
    uint32_t materialization_delay_ms = 0; /* Average length of materialization delay */
    uint64_t last_materialized_lsn = 0;    /* The last materialized LSN (0 if not set) */
    uint32_t verbose = WT_VERBOSE_INFO;    /* Verbose level */
    bool verbose_msg = true;               /* Send verbose messages to msg callback interface */
    bool sql_trace = false;                /* Trace all SQLite calls */
    bool verify = true;                    /* Verify integrity of page delta chains */

    Config() = default;
    Config(WT_EXTENSION_API *wt_api, WT_CONFIG_ARG *config) : extapi(wt_api)
    {
        if (!wt_api || !config)
            throw std::invalid_argument("WT_EXTENSION_API or WT_CONFIG_ARG pointer is null");

        WTConfigParserPtr parser = open_config_parser();

        configure_value(parser.get(), config, "home", home_dir);
        configure_value(parser.get(), config, "cache_size_mb", cache_size_mb);
        configure_value(parser.get(), config, "mmap_size_mb", mmap_size_mb);
        configure_value(parser.get(), config, "delay_ms", delay_ms);
        configure_value(parser.get(), config, "error_ms", error_ms);
        configure_value(parser.get(), config, "force_delay", force_delay);
        configure_value(parser.get(), config, "force_error", force_error);
        configure_value(parser.get(), config, "materialization_delay_ms", materialization_delay_ms);
        configure_value(parser.get(), config, "verbose", verbose);
        configure_value(parser.get(), config, "verbose_msg", verbose_msg);
        configure_value(parser.get(), config, "sql_trace", sql_trace);
        configure_value(parser.get(), config, "verify", verify);
    }

private:
    using WTConfigParserPtr = std::unique_ptr<WT_CONFIG_PARSER, decltype(WT_CONFIG_PARSER::close)>;

    WTConfigParserPtr
    open_config_parser()
    {
        if (!extapi) {
            throw std::invalid_argument("WT_EXTENSION_API pointer is null");
        }

        WT_CONFIG_PARSER *parser = nullptr;
        const char *env_config = getenv("WT_PALITE_CONFIG");
        if (env_config == NULL)
            env_config = "";

        int ret =
          extapi->config_parser_open(extapi, nullptr, env_config, strlen(env_config), &parser);
        if (ret != 0) {
            throw std::runtime_error("Failed to open config parser");
        }

        return WTConfigParserPtr(parser, parser->close);
    }

    template <typename T>
    void
    configure_value(WT_CONFIG_PARSER *parser, WT_CONFIG_ARG *config, const char *key, T &value)
    {
        /* Guard for supported types (bool, integral, string, and filesystem::path types) */
        static_assert(std::is_same_v<T, bool> || std::is_integral_v<T> ||
            std::is_same_v<T, std::string> || std::is_same_v<T, std::filesystem::path>,
          "Unsupported type for configuration");

        auto validate = [](WT_CONFIG_ITEM::WT_CONFIG_ITEM_TYPE type) {
            if constexpr (std::is_same_v<T, bool>) {
                return type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM ||
                  type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL;
            } else if constexpr (std::is_same_v<T, std::string> ||
              std::is_same_v<T, std::filesystem::path>) {
                return type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRING;
            } else { /* integral types */
                return type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM;
            }
        };

        WT_CONFIG_ITEM v{};
        int ret = 0;
        /* Check environment config first, then regular config */
        if ((ret = parser->get(parser, key, &v)) == 0 ||
          (ret = extapi->config_get(extapi, nullptr, config, key, &v)) == 0) {
            if (v.len == 0 || !validate(v.type)) {
                throw std::invalid_argument(std::string("Invalid type for config key: ") + key);
            }

            if constexpr (std::is_same_v<T, std::string>) {
                value = std::string(v.str, v.len);
            } else if constexpr (std::is_same_v<T, std::filesystem::path>) {
                value = std::filesystem::path(std::string(v.str, v.len));
            } else {
                value = static_cast<T>(v.val);
            }
        } else if (ret != WT_NOTFOUND) {
            throw std::runtime_error(std::string("Failed to get config for key: ") + key);
        }
    }
};

template <> struct std::formatter<Config> {
    constexpr auto
    parse(std::format_parse_context &ctx)
    {
        return ctx.begin();
    }

    auto
    format(const Config &cfg, format_context &ctx) const
    {
        return std::format_to(ctx.out(),
          "{{cache_size_mb={:L}, mmap_size_mb={:L}, delay_ms={}, error_ms={}, force_delay={}, "
          "force_error={}, materialization_delay_ms={}, last_materialized_lsn={}, "
          "verbose={}, verbose_msg={}, sql_trace={}, verify={}}}",
          cfg.cache_size_mb, cfg.mmap_size_mb, cfg.delay_ms, cfg.error_ms, cfg.force_delay,
          cfg.force_error, cfg.materialization_delay_ms, cfg.last_materialized_lsn, cfg.verbose,
          cfg.verbose_msg, cfg.sql_trace, cfg.verify);
    }
};

WT_SESSION *const INVALID_PTR = reinterpret_cast<WT_SESSION *>(-1);
constexpr uint64_t INVALID_LSN = UINT64_MAX;

static WT_SESSION *
session(WT_SESSION *s = INVALID_PTR)
{
    thread_local static WT_SESSION *sess = nullptr;
    return s == INVALID_PTR ? sess : (sess = s);
}

template <typename... Args>
void
log(Config &config, WT_VERBOSE_LEVEL level, std::format_string<Args...> fmt, Args &&...args)
{
    if (level > config.verbose)
        return;

    std::string message = std::format(fmt, std::forward<Args>(args)...);
    if (config.verbose_msg) {
        auto api_printf =
          (level <= WT_VERBOSE_WARNING) ? config.extapi->err_printf : config.extapi->msg_printf;
        api_printf(config.extapi, session(), "%s", message.c_str());
    } else {
        std::cerr << message << std::endl;
    }
}

template <typename... Args>
void
log(std::source_location loc, Config &config, WT_VERBOSE_LEVEL level,
  std::format_string<Args...> fmt, Args &&...args)
{
    if (level > config.verbose)
        return;

    thread_local static std::string t_id =
      (std::ostringstream{} << std::this_thread::get_id()).str();
    log(config, level, "[{}] {}:{}: {}: {}", t_id, loc.file_name(), loc.line(), loc.function_name(),
      std::format(fmt, std::forward<Args>(args)...));
}

#define LOG_AT(_lvl, ...)                                                      \
    do {                                                                       \
        if ((_lvl) <= config.verbose)                                          \
            log(std::source_location::current(), config, (_lvl), __VA_ARGS__); \
    } while (0)

#define LOG_ERROR(...) LOG_AT(WT_VERBOSE_ERROR, __VA_ARGS__)
#define LOG_WARN(...) LOG_AT(WT_VERBOSE_WARNING, __VA_ARGS__)
/*#define LOG_NOTICE(...)  LOG_AT(WT_VERBOSE_NOTICE,   __VA_ARGS__) */ /* unused */
/*#define LOG_INFO(...)    LOG_AT(WT_VERBOSE_INFO,     __VA_ARGS__) */ /* unused */
#define LOG_DEBUG(...) LOG_AT(WT_VERBOSE_DEBUG_1, __VA_ARGS__)
#define LOG_DIAG(...) LOG_AT(WT_VERBOSE_DEBUG_2, __VA_ARGS__)
#define LOG_TRACE(...) LOG_AT(WT_VERBOSE_DEBUG_3, __VA_ARGS__)

#define LOG_SQL_TRACE(str)       \
    do {                         \
        if (config.sql_trace)    \
            LOG_DIAG("{}", str); \
    } while (0)

/* Exception-safe template method that catches C++ exceptions */
template <typename T, typename S, typename MemberFunc, typename... Args>
static int
safe_call(WT_SESSION *sess, S *api, MemberFunc func, Args &&...args)
{
    if (!api) {
        return EINVAL;
    }

    session(sess);
    /* std::unique_ptr is used as a simple scope guard to reset the session upon function exit */
    std::unique_ptr<WT_SESSION, std::function<decltype(session)>> reset_session{nullptr, &session};

    T *obj = static_cast<T *>(api);
    Config &config = obj->config; /* needed for logging macros bellow */
    try {
        return std::invoke(func, obj, std::forward<Args>(args)...);
    } catch (const std::bad_alloc &e) {
        LOG_ERROR("Memory allocation failed: {}", e.what());
        return ENOMEM;
    } catch (const std::invalid_argument &e) {
        LOG_ERROR("Invalid argument: {}", e.what());
        return EINVAL;
    } catch (const std::filesystem::filesystem_error &e) {
        LOG_ERROR("Filesystem error: {}", e.what());
        return e.code().value();
    } catch (const std::system_error &e) {
        LOG_ERROR("System error: {}", e.what());
        return e.code().value();
    } catch (const std::runtime_error &e) {
        LOG_ERROR("Runtime error: {}", e.what());
        return EINVAL;
    } catch (const std::exception &e) {
        LOG_ERROR("Exception: {}", e.what());
        return EINVAL;
    } catch (...) {
        LOG_ERROR("Unknown error occurred");
        return EFAULT;
    }
}

template <typename Exception = std::runtime_error, typename... Args>
void
log_and_throw(
  std::source_location loc, Config &config, std::format_string<Args...> fmt, Args &&...args)
{
    log(loc, config, WT_VERBOSE_ERROR, fmt, std::forward<Args>(args)...);

    std::string message = std::format("{}:{}: {}: {}", loc.file_name(), loc.line(),
      loc.function_name(), std::format(fmt, std::forward<Args>(args)...));
    throw Exception(message);
}

#define LOG_AND_THROW(...) log_and_throw(std::source_location::current(), config, __VA_ARGS__)

/*
 * Generic formatter for custom pointer types. Excludes char* and void* specializations provided by
 * the standard library.
 */
template <typename T>
concept custom_ptr_t =
  !std::is_same_v<std::remove_cv_t<T>, char> && !std::is_void_v<std::remove_cv_t<T>>;

template <custom_ptr_t T> struct std::formatter<T *, char> {
    /* Use the same format specifiers as for uintptr_t */
    std::formatter<std::uintptr_t, char> underlying;

    constexpr auto
    parse(std::format_parse_context &ctx)
    {
        return underlying.parse(ctx);
    }

    auto
    format(T *ptr, std::format_context &ctx) const
    {
        return underlying.format(reinterpret_cast<std::uintptr_t>(ptr), ctx);
    }
};

/* SQLite3 exec helper */
class SQLiteCall {
    Config &config;
    sqlite3 *db;
    std::source_location loc;
    const char *const func_name;

    /*
     * Build format string at compile time. It contains the fixed prefix followed by N occurrences
     * of the pattern `'{}', ` with the final comma+space trimmed.
     */
    template <std::size_t N, std::size_t K>
    static consteval auto
    make_log_format(const char (&prefix)[K])
    {
        const std::size_t prefix_len = sizeof(prefix) - 1; /* exclude null terminator */
        /* Each argument contributes: 4 chars for "'{}'" plus 2 chars for ", " */
        const std::size_t braces_len = N == 0 ? 0 : (N * 6);
        std::array<char, prefix_len + braces_len + 1> fmt{};

        std::copy_n(prefix, prefix_len, fmt.begin()); /* excludes null terminator */

        const char c[] = "'{}', ";
        for (size_t i = prefix_len; i != fmt.size() - 1; ++i)
            fmt[i] = c[(i - prefix_len) % 6];
        fmt[fmt.size() - 3] = '\0';

        return fmt;
    }

    template <typename R, typename... Args>
    std::string
    trace_sqlite3_call(R ret, Args &&...args)
    {
        /* Build format string with fixed prefix and N occurrences of '{}' */
        static constexpr auto fmt_arr =
          make_log_format<sizeof...(Args)>("{}; return: {} ({}); details: {} ({}); args: ");

        const char *err_msg = "";
        if (ret != SQLITE_OK && db != nullptr) {
            err_msg = sqlite3_errmsg(db);
        }

        auto err_cstr = sqlite3_errstr(ret);
        auto ext_err = sqlite3_extended_errcode(db);
        auto fargs = std::make_format_args(func_name, ret, err_cstr, ext_err, err_msg, args...);

        return std::vformat(fmt_arr.data(), fargs);
    }

public:
    SQLiteCall(Config &cfg, sqlite3 *d, std::source_location l, const char *func)
        : config(cfg), db(d), loc(l), func_name(func)
    {
    }

    enum Exec { THROW, NO_THROW };

    template <Exec Policy = THROW, typename Func, typename... Args>
    decltype(auto)
    exec(Func &&c_func, Args &&...args)
    {
        auto ret = std::invoke(std::forward<Func>(c_func), std::forward<Args>(args)...);
        LOG_SQL_TRACE(trace_sqlite3_call(ret, args...));

        if (ret != SQLITE_OK && ret != SQLITE_ROW && ret != SQLITE_DONE) {
            std::string error_msg = trace_sqlite3_call(ret, args...);
            log(loc, config, WT_VERBOSE_ERROR, "{}", error_msg);

            enforce<Policy>(ret, error_msg);
        }

        return ret;
    }

    template <Exec Policy>
    void
    enforce(auto ret, const std::string &error_msg)
    {
        if constexpr (Policy != THROW)
            return;

        static constexpr auto sqlite2errno = []() constexpr
        {
            std::array<std::errc, SQLITE_NOTADB + 1> a{};
            a[SQLITE_OK] = std::errc::operation_not_permitted; /* unused */
            a[SQLITE_ERROR] = std::errc::invalid_argument;
            a[SQLITE_INTERNAL] = std::errc::invalid_argument;
            a[SQLITE_PERM] = std::errc::permission_denied;
            a[SQLITE_ABORT] = std::errc::operation_canceled;
            a[SQLITE_BUSY] = std::errc::device_or_resource_busy;
            a[SQLITE_LOCKED] = std::errc::resource_unavailable_try_again;
            a[SQLITE_NOMEM] = std::errc::not_enough_memory;
            a[SQLITE_READONLY] = std::errc::read_only_file_system;
            a[SQLITE_INTERRUPT] = std::errc::interrupted;
            a[SQLITE_IOERR] = std::errc::io_error;
            a[SQLITE_CORRUPT] = std::errc::illegal_byte_sequence;
            a[SQLITE_NOTFOUND] = std::errc::no_such_file_or_directory;
            a[SQLITE_FULL] = std::errc::no_space_on_device;
            a[SQLITE_CANTOPEN] = std::errc::io_error;
            a[SQLITE_PROTOCOL] = std::errc::protocol_error;
            a[SQLITE_EMPTY] = std::errc::no_message;
            a[SQLITE_SCHEMA] = std::errc::invalid_argument;
            a[SQLITE_TOOBIG] = std::errc::file_too_large;
            a[SQLITE_CONSTRAINT] = std::errc::operation_not_permitted;
            a[SQLITE_MISMATCH] = std::errc::invalid_argument;
            a[SQLITE_MISUSE] = std::errc::invalid_argument;
            a[SQLITE_NOLFS] = std::errc::function_not_supported;
            a[SQLITE_AUTH] = std::errc::permission_denied;
            a[SQLITE_FORMAT] = std::errc::illegal_byte_sequence;
            a[SQLITE_RANGE] = std::errc::result_out_of_range;
            a[SQLITE_NOTADB] = std::errc::illegal_byte_sequence;
            return a;
        }
        ();

        /* Verify that each slot was assigned. */
        static_assert(
          std::ranges::none_of(sqlite2errno, [](std::errc e) { return static_cast<int>(e) == 0; }),
          "Uninitialized SQLite error code found");

        if (ret < std::size(sqlite2errno))
            throw std::system_error(std::make_error_code(sqlite2errno[ret]), error_msg);
        else
            throw std::runtime_error(error_msg);
    }
};

#define SQL_CALL_CHECK(db, func, ...) \
    SQLiteCall(config, db, std::source_location::current(), #func).exec(func, __VA_ARGS__)

#define SQL_CALL_CHECK_NO_THROW(db, func, ...)                     \
    SQLiteCall(config, db, std::source_location::current(), #func) \
      .exec<SQLiteCall::NO_THROW>(func, __VA_ARGS__)

#define SQL_CALL_OPEN(func, ...) \
    SQLiteCall(config, nullptr, std::source_location::current(), #func).exec(func, __VA_ARGS__)

/* Requires a Connection object to be available as 'conn'. */
#define SQ_CHECK(func, ...) SQL_CALL_CHECK(conn.db_instance(), func, __VA_ARGS__)

/*
 * SQLite database connection. Manages opening/closing the database and preparing statements.
 */
class Connection {
    Config &config;
    sqlite3 *db = nullptr;
    std::vector<sqlite3_stmt *> statements;

    /* Common configuration parameters for connections */
    constexpr static std::string_view config_statements[] = {
      /*
       * The WAL journaling mode uses a write-ahead log instead of a rollback journal to implement
       * transactions. This significantly improves performance.
       */
      "PRAGMA journal_mode = WAL;",

      /*
       * Turn Synchronous mode OFF for better performance. We don't care about database corruption
       * in case of OS crash or power failure.
       */
      "PRAGMA synchronous = OFF;",

      /* For temporary store use memory instead of disk. */
      "PRAGMA temp_store = MEMORY;",

      /* Set busy timeout to 10 seconds. */
      "PRAGMA busy_timeout = 10000;"};

public:
    using StatementPtr = std::unique_ptr<sqlite3_stmt, std::function<decltype(sqlite3_reset)>>;

    ~Connection() = default;
    Connection(Config &cfg, const std::filesystem::path &db_path)
        : config(cfg), db(open_db(cfg, db_path))
    {
        configure(Connection::config_statements);
    }

    StatementPtr
    db_statement(size_t idx)
    {
        return StatementPtr(statements[idx], [this](sqlite3_stmt *s) {
            /* do not throw from d'tor */
            return SQL_CALL_CHECK_NO_THROW(db, sqlite3_reset, s);
        });
    }

    sqlite3 *
    db_instance() const
    {
        return db;
    }

    template <typename Container>
    void
    configure(const Container &cfg_statements)
    {
        for (const auto &stmt : cfg_statements) {
            SQL_CALL_CHECK(db, sqlite3_exec, db, stmt.data(), nullptr, nullptr, nullptr);
        }
    }

    template <typename Container>
    void
    prepare_statements(const Container &sql_statements)
    {
        statements.clear();
        for (const auto &stmt : sql_statements) {
            sqlite3_stmt *prepared_stmt = nullptr;
            SQL_CALL_CHECK(db, sqlite3_prepare_v2, db, stmt.data(), -1, &prepared_stmt, nullptr);
            statements.push_back(prepared_stmt);
        }
    }

    void
    close()
    {
        finalize_statements();
        close_db();
    }

private:
    static sqlite3 *
    open_db(Config &config, const std::filesystem::path &db_path)
    {
        const unsigned flags =
          /*
           * The database is opened for reading and writing, and is created if it does not already
           * exist.
           */
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
          /*
           * Use the "multi-thread" threading mode. Cannot share this connection between threads.
           */
          SQLITE_OPEN_NOMUTEX;

        sqlite3 *pdb = nullptr;
        SQL_CALL_OPEN(sqlite3_open_v2, db_path.c_str(), &pdb, flags, nullptr);
        LOG_DEBUG("Opened SQLite database: {} ({})", pdb, db_path.c_str());

        return pdb;
    }

    void
    finalize_statements()
    {
        std::ranges::for_each(
          statements, [this](sqlite3_stmt *s) { SQL_CALL_CHECK(db, sqlite3_finalize, s); });
        statements.clear();
    }

    void
    close_db()
    {
        int ret = sqlite3_close(db);
        if (ret == SQLITE_OK) {
            LOG_DEBUG("Closed SQLite database: {}", db);
        } else {
            LOG_ERROR(
              "Failed to close SQLite database: {}. Error: {} ({})", db, ret, sqlite3_errstr(ret));
        }
        db = nullptr;
    }
};

/*
 * Generic functionality of a database table. Manages connections per thread and provides access
 * control.
 */
template <typename Traits> class Table {
protected:
    Config &config;
    std::shared_mutex &store_access; /* Store-wide access mutex */

    std::filesystem::path table_file;

    std::once_flag create_table;
    std::shared_mutex conn_state; /* protects 'connections' map */
    std::unordered_map<std::thread::id, std::unique_ptr<Connection>> connections;

public:
    std::shared_mutex access; /* readers-writer mutex for table */

    ~Table() = default;
    Table(Config &cfg, std::shared_mutex &str_access, const std::filesystem::path &file)
        : config(cfg), store_access(str_access), table_file(file)
    {
    }

    void
    close()
    {
        std::ranges::for_each(connections, [](auto &kv) { kv.second->close(); });
        connections.clear();
    }

    enum AccessMode { READ, WRITE, BYPASS };

protected:
    class Access {
        std::shared_lock<std::shared_mutex> store_lock;
        std::shared_lock<std::shared_mutex> table_read_lock;
        std::unique_lock<std::shared_mutex> table_write_lock;

    public:
        Connection &conn;

        ~Access() = default;
        Access(AccessMode mode, Connection &cn, std::shared_mutex &store_access,
          std::shared_mutex &table_access)
            : conn(cn)
        {
            switch (mode) {
            case AccessMode::READ:
                store_lock = std::shared_lock(store_access);
                table_read_lock = std::shared_lock(table_access);
                break;
            case AccessMode::WRITE:
                store_lock = std::shared_lock(store_access);
                table_write_lock = std::unique_lock(table_access);
                break;
            case AccessMode::BYPASS:
                /* no locking */
                break;
            default:
                assert(false && "Invalid AccessMode");
            }
        }

        Access(const Access &) = delete;
        Access &operator=(const Access &) = delete;
    };

    Access
    request(AccessMode mode)
    {
        return Access{mode, conn(), store_access, access};
    }

private:
    Connection &
    conn()
    {
        {
            std::shared_lock read_lock(conn_state);
            auto it = connections.find(std::this_thread::get_id());
            if (it != connections.end())
                return *(it->second);
        }

        {
            std::unique_lock write_lock(conn_state);
            auto [it, inserted] = connections.try_emplace(
              std::this_thread::get_id(), std::make_unique<Connection>(config, table_file));

            if (!inserted)
                LOG_AND_THROW("Connection already exists for this thread");

            Connection &new_conn = *(it->second);
            new_conn.configure(static_cast<Traits *>(this)->conn_config());

            std::call_once(
              create_table,
              [this](Connection &c) {
                  c.configure(Traits::create_statements);
                  LOG_DEBUG("SQLite database schema initialized: {}", c.db_instance());
              },
              new_conn);

            new_conn.prepare_statements(Traits::sql_statements);

            return new_conn;
        }
    }
};

/*
 * Specific Tables
 */

struct Globals : public Table<Globals> {
    constexpr static std::string_view prefix = "globals";

    enum Statement {
        MAKE_NEXT_LSN,
        GET_LAST_LSN,
        ADD_TABLE_ID,
        GET_TABLE_IDS,
        COUNT /* number of statements */
    };

    constexpr static auto sql_statements = []() constexpr
    {
        std::array<std::string_view, COUNT> stmt{};

        /* Increment LSN. */
        stmt[MAKE_NEXT_LSN] =
          R"(UPDATE globals
             SET val = val + 1
             WHERE id = 0
             RETURNING val;)";

        /* Get the last LSN. */
        stmt[GET_LAST_LSN] =
          R"(SELECT val
             FROM globals
             WHERE id = 0;)",

        /* Add a new table ID if it does not already exist. */
          stmt[ADD_TABLE_ID] =
            R"(INSERT OR IGNORE INTO globals (id, val)
               VALUES (1, ?);)";

        /* Get all known table IDs. */
        stmt[GET_TABLE_IDS] =
          R"(SELECT val
             FROM globals
             WHERE id = 1;)";

        return stmt;
    }
    ();

    static_assert(
      std::ranges::none_of(Globals::sql_statements, [](const auto &s) { return s.empty(); }),
      "Not all SQL statements were initialized");

    constexpr static std::string_view create_statements[] = {
      /*
       * Globals table to store LSN counter and known table IDs.
       *  - LSN counter id = 0
       *  - Table IDs id = 1
       */
      R"(CREATE TABLE IF NOT EXISTS globals (
            id INTEGER CHECK (id = 0 OR id = 1),
            val INTEGER NOT NULL,
         PRIMARY KEY (id, val));)",

      /* Init LSN counter, if the table is empty; do nothing otherwise. */
      R"(INSERT INTO globals (id, val)
         SELECT 0, 0
         WHERE NOT EXISTS (SELECT 1 FROM globals WHERE id = 0);)"};

    ~Globals() = default;
    Globals(Config &cfg, std::shared_mutex &store_access, const std::filesystem::path &home)
        : Table<Globals>(cfg, store_access, home / std::format("{}.db", Globals::prefix))
    {
    }

    Globals(const Globals &) = delete;
    Globals &operator=(const Globals &) = delete;

    auto
    conn_config() -> std::ranges::view auto
    {
        /* No special configuration is required for the 'globals' table. */
        return std::views::empty<std::string_view>;
    }

    uint64_t
    fetch_lsn(Connection &conn, Statement stmt_type)
    {
        Connection::StatementPtr stmt = conn.db_statement(stmt_type);
        SQ_CHECK(sqlite3_step, stmt.get());

        return sqlite3_column_int64(stmt.get(), 0);
    }

    uint64_t
    make_next_lsn()
    {
        auto acc = request(AccessMode::WRITE);

        return fetch_lsn(acc.conn, Statement::MAKE_NEXT_LSN);
    }

    uint64_t
    get_last_lsn()
    {
        auto acc = request(AccessMode::READ);

        return fetch_lsn(acc.conn, Statement::GET_LAST_LSN);
    }

    void
    add_table_id(uint64_t table_id)
    {
        auto acc = request(AccessMode::WRITE);
        Connection &conn = acc.conn;

        Connection::StatementPtr stmt = conn.db_statement(Statement::ADD_TABLE_ID);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, static_cast<sqlite3_int64>(table_id));
        SQ_CHECK(sqlite3_step, stmt.get());
    }

    std::vector<uint64_t>
    get_table_ids(AccessMode mode = AccessMode::READ)
    {
        auto acc = request(mode);
        Connection &conn = acc.conn;

        Connection::StatementPtr stmt = conn.db_statement(Statement::GET_TABLE_IDS);
        std::vector<uint64_t> table_ids;
        while (SQ_CHECK(sqlite3_step, stmt.get()) == SQLITE_ROW) {
            table_ids.push_back(static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0)));
        }

        return table_ids;
    }
};

struct Checkpoints : public Table<Checkpoints> {
    constexpr static std::string_view prefix = "checkpoints";

    enum Statement {
        PUT_CHECKPOINT,
        GET_CHECKPOINT,
        DELETE_CHECKPOINTS,
        COUNT /* number of statements */
    };

    constexpr static auto sql_statements = []() constexpr
    {
        std::array<std::string_view, COUNT> stmt{};

        /*
         * Insert a new checkpoint.
         */
        stmt[PUT_CHECKPOINT] =
          R"(INSERT INTO checkpoints (lsn, timestamp, checkpoint_metadata)
             VALUES (?, ?, ?);)";

        /*
         * Get the latest checkpoint (i.e., checkpoint with the highest lsn) if query lsn equals to
         * WT_PAGE_LOG_LSN_MAX ( passed as parameter: ?2); otherwise get the checkpoint with the
         * given lsn.
         */
        stmt[GET_CHECKPOINT] =
          R"(SELECT lsn, timestamp, checkpoint_metadata
             FROM checkpoints
             WHERE (?1 = ?2 OR lsn = ?1)
             ORDER BY
                 lsn DESC,
                 timestamp DESC
             LIMIT 1;)";

        /*
         * Delete all checkpoints with lsn greater than the given lsn.
         */
        stmt[DELETE_CHECKPOINTS] =
          R"(DELETE FROM checkpoints
             WHERE lsn > ?;)";

        return stmt;
    }
    ();

    static_assert(
      std::ranges::none_of(Checkpoints::sql_statements, [](const auto &s) { return s.empty(); }),
      "Not all SQL statements were initialized");

    constexpr static std::string_view create_statements[] = {
      R"(CREATE TABLE IF NOT EXISTS checkpoints (
            lsn INTEGER NOT NULL,
            timestamp INTEGER NOT NULL,
            checkpoint_metadata BLOB,
         PRIMARY KEY (lsn, timestamp));)"};

    ~Checkpoints() = default;
    Checkpoints(Config &cfg, std::shared_mutex &store_access, const std::filesystem::path &home)
        : Table<Checkpoints>(cfg, store_access, home / std::format("{}.db", Checkpoints::prefix))
    {
    }

    Checkpoints(const Checkpoints &) = delete;
    Checkpoints &operator=(const Checkpoints &) = delete;

    auto
    conn_config() -> std::ranges::view auto
    {
        /* No special configuration is required for the 'checkpoints' table. */
        return std::views::empty<std::string_view>;
    }

    void
    put(uint64_t lsn, uint64_t checkpoint_timestamp, const WT_ITEM *checkpoint_metadata)
    {
        auto acc = request(AccessMode::WRITE);
        Connection &conn = acc.conn;

        Connection::StatementPtr stmt = conn.db_statement(Statement::PUT_CHECKPOINT);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, lsn);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 2, checkpoint_timestamp);
        SQ_CHECK(sqlite3_bind_blob, stmt.get(), 3, checkpoint_metadata->data,
          checkpoint_metadata->size, SQLITE_STATIC);
        SQ_CHECK(sqlite3_step, stmt.get());
    }

    int
    get(uint64_t &lsn, uint64_t *timestamp, WT_ITEM *checkpoint_metadata,
      AccessMode mode = AccessMode::READ)
    {
        auto acc = request(mode);
        Connection &conn = acc.conn;

        Connection::StatementPtr stmt = conn.db_statement(Statement::GET_CHECKPOINT);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, static_cast<sqlite3_int64>(lsn));
        SQ_CHECK(
          sqlite3_bind_int64, stmt.get(), 2, static_cast<sqlite3_int64>(WT_PAGE_LOG_LSN_MAX));
        int ret = SQ_CHECK(sqlite3_step, stmt.get());
        if (ret == SQLITE_DONE) {
            /* No checkpoint found */
            if (timestamp)
                *timestamp = 0;

            if (checkpoint_metadata) {
                checkpoint_metadata->data = nullptr;
                checkpoint_metadata->size = 0;
            }

            return WT_NOTFOUND;
        }

        lsn = sqlite3_column_int64(stmt.get(), 0);
        if (timestamp)
            *timestamp = sqlite3_column_int64(stmt.get(), 1);

        if (checkpoint_metadata) {
            const void *blob = sqlite3_column_blob(stmt.get(), 2);
            int size = sqlite3_column_bytes(stmt.get(), 2);
            fill_item(checkpoint_metadata, blob, static_cast<size_t>(size));
        }

        return 0;
    }

    void
    delete_many(uint64_t lsn, AccessMode mode = AccessMode::WRITE)
    {
        auto acc = request(mode);
        Connection &conn = acc.conn;

        Connection::StatementPtr stmt = conn.db_statement(Statement::DELETE_CHECKPOINTS);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, static_cast<sqlite3_int64>(lsn));
        SQ_CHECK(sqlite3_step, stmt.get());
    }
};

struct PageInfo {
    uint64_t table_id;
    uint64_t page_id;
    uint64_t lsn;
    uint64_t backlink_lsn;
    uint64_t base_lsn;
    uint32_t flags;
    WT_PAGE_LOG_ENCRYPTION encryption;
};

template <> struct std::formatter<PageInfo> {
    constexpr auto
    parse(std::format_parse_context &ctx)
    {
        return ctx.begin();
    }

    auto
    format(const PageInfo &page, format_context &ctx) const
    {
        /* TODO: print encryption if special formatting is given, e.g. {:e} */
        return std::format_to(ctx.out(),
          "{{table_id={}, page_id={}, lsn={}, backlink_lsn={}, base_lsn={}, "
          "flags={:#x}}}",
          page.table_id, page.page_id, page.lsn, page.backlink_lsn, page.base_lsn, page.flags);
    }
};

struct Pages : public Table<Pages> {
    constexpr static std::string_view prefix = "pages";

    enum Statement {
        PUT_PAGE,
        GET_PAGE_IDS,
        GET_FULL_PAGE_LSN,
        GET_PAGE_INFOS,
        GET_PAGES,
        DELETE_PAGES,
        COUNT /* number of statements */
    };

    /*
     * Our flags start at the 16th bit (0x10000u) to avoid conflicts with WT_PAGE_LOG_PUT_ARGS
     * flags.
     */
    static constexpr uint32_t WT_PAGE_LOG_DISCARDED = 0x10000u;

    constexpr static auto sql_statements = []() constexpr
    {
        std::array<std::string_view, COUNT> stmt{};

        /*
         * Insert new pages or replace write failures.
         *
         * When writing a new delta page, if a delta page already exists with the same (table_id,
         * page_id, backlink_lsn), the existing page is treated as a write failure and replaced.
         *
         * This uniqueness constraint is enforced by a partial index that applies only to delta
         * pages. See the CREATE TABLE statement for the 'pages' table.
         */
        stmt[PUT_PAGE] =
          R"(INSERT OR REPLACE INTO pages (
                  table_id,
                  page_id,
                  lsn,
                  backlink_lsn,
                  base_lsn,
                  flags,
                  encryption,
                  timestamp_materialized_us,
                  page_data)
             VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);)";

        /*
         * Get all unique page IDs for a given table that have their latest page record with LSN <=
         * query LSN and not discarded.
         */
        stmt[GET_PAGE_IDS] =
          R"(SELECT DISTINCT p1.page_id
             FROM pages AS p1
             WHERE p1.table_id = ?1
                 AND p1.lsn <= ?2
                 AND NOT EXISTS (
                     SELECT 1
                     FROM pages AS p2
                     WHERE p2.table_id = p1.table_id
                         AND p2.page_id = p1.page_id
                         AND p2.lsn <= ?2
                         AND p2.discarded = 1
                 );)";

        /*
         * Get the LSN of the first non-discarded full page below the given LSN.
         *
         * The query LSN may belong to a delta page. Therefore, we find the corresponding full page
         * LSN first. Then we look for the previous full page LSN. Assuming that given query LSN
         * does not belong to discarded page.
         *
         * This is used with full page backlink verification.
         */
        stmt[GET_FULL_PAGE_LSN] =
          R"(WITH full_page AS (
             SELECT MAX(p0.lsn) AS lsn
             FROM pages AS p0
             WHERE p0.table_id = ?1
                 AND p0.page_id = ?2
                 AND p0.lsn <= ?3
                 AND p0.delta = 0
             )
             SELECT MAX(p1.lsn)
             FROM
                 pages AS p1,
                 full_page AS fp
             WHERE p1.table_id = ?1
                 AND p1.page_id = ?2
                 AND p1.lsn < fp.lsn
                 AND p1.delta = 0
                 AND NOT EXISTS (
                     SELECT 1
                     FROM pages AS p2
                     WHERE p2.table_id = p1.table_id
                         AND p2.page_id = p1.page_id
                         AND p2.lsn < fp.lsn
                         AND p2.discarded = 1
                 );)";

        /*
         * Retrieve information about entire delta chain
         * stopping at the first full page. The chain may include discarded page.
         *
         * This query uses a window function OVER to assign a group number
         * to rows based on the 'delta' column. This is more performant
         * than discovering full page in a separate query as it may only
         * require a single scan over the data.
         *
         * The window function works as follows:
         *
         * First, the inner part:
         *     CASE
         *         WHEN delta = 0 THEN 1
         *         ELSE 0
         *     END
         *
         * This expression looks at each row one by one. For delta pages
         * (where delta = 1), it produces a 0. For full pages (where
         * delta = 0), it produces a 1.
         *
         * So, it's essentially a marker for the rows with full pages.
         * We consider such rows as terminators.
         *
         * Next, the outer part:
         *     SUM(...) OVER (ORDER BY lsn DESC) as page_group
         *
         * This expression takes the markers produced by the inner part
         * and computes a running total (SUM) of these markers, ordered
         * by LSN in descending order (from newest to oldest).
         *
         * This creates a cumulative count of how many "terminating" rows
         * (delta = 0) have been seen so far.
         *
         * Example:
         *
         *   lsn | delta | inner CASE | page_group | explanation
         *   ----+-------+------------+------------+------------
         *   10  | 1     | 0          | 0          | Sum of (0)
         *   9   | 1     | 0          | 0          | Sum of (0, 0)
         *   8   | 1     | 0          | 0          | Sum of (0, 0, 0)
         *   7   | 0     | 1          | 1          | Sum of (0, 0, 0, 1)
         *   6   | 0     | 1          | 2          | Sum of (0, 0, 0, 1, 1)
         *
         * The entire chain of pages (lsn 10, 9, 8, 7) is returned, stopping
         * at the first full page (lsn 7). The next full page (lsn 6) is
         * not included because page_group becomes 2 and we explicitly check
         * for that:
         *     ...
         *     WHERE page_group = 0
         *         OR (page_group = 1 AND delta = 0)
         *
         * This condition ensures that we get all delta pages in the chain
         * up to the first full page.
         */
        stmt[GET_PAGE_INFOS] =
          R"(WITH chained_pages AS (
             SELECT
                 table_id,
                 page_id,
                 lsn,
                 backlink_lsn,
                 base_lsn,
                 flags,
                 delta,
                 SUM(CASE
                         WHEN delta = 0 THEN 1
                         ELSE 0
                     END) OVER (ORDER BY lsn DESC) as page_group
             FROM pages
             WHERE table_id = ?
                 AND page_id = ?
                 AND lsn <= ?
                 AND timestamp_materialized_us <= ?
             )
             SELECT
                 table_id,
                 page_id,
                 lsn,
                 backlink_lsn,
                 base_lsn,
                 flags
             FROM chained_pages
             WHERE page_group = 0
                 OR (page_group = 1 AND delta = 0)
             ORDER BY lsn DESC;)";

        /*
         * Simple selection of all pages below the given LSN. Verification of the delta chain and
         * stopping at the terminating full page is done in code.
         */
        stmt[GET_PAGES] =
          R"(SELECT
                  lsn,
                  backlink_lsn,
                  base_lsn,
                  flags,
                  encryption,
                  page_data
             FROM pages
             WHERE table_id = ?
                 AND page_id = ?
                 AND lsn <= ?
                 AND timestamp_materialized_us <= ?
             ORDER BY lsn DESC;)";

        /* Remove all pages above the given LSN. */
        stmt[DELETE_PAGES] =
          R"(DELETE FROM pages
             WHERE lsn > ?;)";

        return stmt;
    }
    ();

    static_assert(
      std::ranges::none_of(Pages::sql_statements, [](const auto &s) { return s.empty(); }),
      "SQL statements must not be empty");

    /* These flags used below in generated columns for 'pages' table. */
    static_assert(WT_PAGE_LOG_DELTA == 0x2, "WT_PAGE_LOG_DELTA value changed");
    static_assert(WT_PAGE_LOG_DISCARDED == 0x10000, "WT_PAGE_LOG_DISCARDED value changed");

    constexpr static std::string_view create_statements[] = {
      R"(CREATE TABLE IF NOT EXISTS pages (
             table_id INTEGER NOT NULL,
             page_id INTEGER NOT NULL,
             lsn INTEGER NOT NULL,
             backlink_lsn INTEGER NOT NULL,
             base_lsn INTEGER NOT NULL,
             flags INTEGER NOT NULL,
             delta INTEGER AS ((flags & 0x2) != 0) VIRTUAL, -- WT_PAGE_LOG_DELTA
             discarded INTEGER AS ((flags & 0x10000) != 0) VIRTUAL, -- WT_PAGE_LOG_DISCARDED
             encryption STRING NOT NULL,
             timestamp_materialized_us INTEGER NOT NULL,
             page_data BLOB,
         PRIMARY KEY (table_id, page_id, lsn));)",

      /*
       * This index exists for the case in which we have written a page
       * delta and then need to write it again.
       *
       * This generally happens in eviction. We have a base page already written
       * and we now try to evict it. We finished writing a delta to disk but
       * then we fail the reconciliation. It is a delta so we cannot explicitly
       * free the write with the same page id. After a while, we retry the
       * eviction and this time we writes a delta based on the same base page.
       * Thus this write will have the same backlink_lsn to the previous write.
       *
       * At this stage, there may be no checkpoint running so discarding the
       * unfinished checkpoint doesn't help in this case. It is also possible
       * that these two writes will be included in the same checkpoint that is
       * not discarded.
       *
       * To gracefully handle write failures of delta pages we will keep
       * track of the backlink_lsn within each delta chain. Tracking is done via
       * a *partial* unique index on (table_id, page_id, backlink_lsn) for
       * genuine delta pages only: delta=1 AND discarded=0.
       * (Partial index can include only a subset of rows in the table.)
       *
       * The primary key for the table (table_id, page_id, lsn) does not help
       * because lsn is always increasing, making each record unique.
       *
       * Including backlink_lsn in the primary key would not work because
       * together with always increasing lsn the constraint is always satisfied.
       *
       * Hence, we need a separate unique index for delta pages only (excluding
       * discarded pages, which are a special case).
       *
       * Example:
       *
       *   table_id | page_id | lsn | backlink_lsn | base_lsn | delta
       *   ---------+---------+-----+--------------+----------+------
       *       1    |   100   | 5   |      0       |   0      |   0
       *       1    |   100   | 6   |      5       |   5      |   1
       *       1    |   100   | 7   |      6       |   5      |   1
       *       1    |   100   | 8   |      7       |   5      |   1    <-- write failure
       *       1    |   100   | 9   |      7       |   5      |   1    <-- new delta page
       *
       * Suppose, the page with lsn=8 is a write failure. When a new delta page
       * is written with (lsn=9, backlink_lsn=7, base_lsn=5), then it will
       * conflict with the existing page with lsn=8 because they have the same
       * (table_id, page_id, backlink_lsn). The partial index will ensure
       * that there is at most one such delta page.
       *
       * The new page with lsn=9 will replace the failed page with lsn=8 because
       * pages are inserted with 'INSERT OR REPLACE INTO pages'.
       *
       * See also PUT_PAGE statement.
       */
      R"(CREATE UNIQUE INDEX IF NOT EXISTS ux_delta
         ON pages (table_id, page_id, backlink_lsn)
         WHERE delta = 1 AND discarded = 0;)",
    };

    ~Pages() = default;
    Pages(Config &cfg, std::shared_mutex &store_access, const std::filesystem::path &home,
      uint64_t table_id)
        : Table<Pages>(
            cfg, store_access, home / std::format("{}_{:06}.db", Pages::prefix, table_id))
    {
    }

    Pages(const Pages &) = delete;
    Pages &operator=(const Pages &) = delete;

    auto
    conn_config() -> std::ranges::view auto
    {
        const uint64_t MMAP_SIZE = config.mmap_size_mb * 1_MB;
        const uint64_t PAGE_SIZE = 16_KB;
        const uint64_t CACHE_PAGES = (config.cache_size_mb * 1_MB) / PAGE_SIZE;

        const static std::string config_statements[] = {
          /*
           * Uses memory mapping instead of read/write calls when the database is < mmap_size in
           * bytes.
           */
          std::format("PRAGMA mmap_size = {};", MMAP_SIZE),

          /*
           * Increase page size to 16KB (default is 4KB). This improves performance for tables with
           * BLOBs.
           */
          std::format("PRAGMA page_size = {};", PAGE_SIZE),

          /*
           * Set cache size as configured. Cache size is specified in megabytes. Convert to number
           * of pages.
           */
          std::format("PRAGMA cache_size = {};", CACHE_PAGES)};

        return std::views::all(config_statements);
    }

    void
    put(uint64_t table_id, uint64_t page_id, uint64_t lsn, WT_PAGE_LOG_PUT_ARGS *args,
      const WT_ITEM *buf)
    {
        {
            auto acc_w = request(AccessMode::WRITE);
            Connection &conn = acc_w.conn;

            Connection::StatementPtr stmt = conn.db_statement(Statement::PUT_PAGE);
            SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, static_cast<sqlite3_int64>(table_id));
            SQ_CHECK(sqlite3_bind_int64, stmt.get(), 2, static_cast<sqlite3_int64>(page_id));
            SQ_CHECK(sqlite3_bind_int64, stmt.get(), 3, static_cast<sqlite3_int64>(lsn));
            SQ_CHECK(
              sqlite3_bind_int64, stmt.get(), 4, static_cast<sqlite3_int64>(args->backlink_lsn));
            SQ_CHECK(sqlite3_bind_int64, stmt.get(), 5, static_cast<sqlite3_int64>(args->base_lsn));
            SQ_CHECK(sqlite3_bind_int64, stmt.get(), 6, static_cast<sqlite3_int64>(args->flags));
            SQ_CHECK(sqlite3_bind_text, stmt.get(), 7, args->encryption.dek,
              strlen(args->encryption.dek), SQLITE_STATIC);
            SQ_CHECK(sqlite3_bind_int64, stmt.get(), 8,
              static_cast<sqlite3_int64>(now_us() + (config.materialization_delay_ms * 1ms / 1us)));
            SQ_CHECK(sqlite3_bind_blob, stmt.get(), 9, buf->data, buf->size, SQLITE_STATIC);
            SQ_CHECK(sqlite3_step, stmt.get());
        }

        if (config.verify) {
            auto acc_r = request(AccessMode::READ);
            verify_chain(acc_r.conn, table_id, page_id, lsn);
        }
    }

    void
    get(uint64_t table_id, uint64_t page_id, WT_PAGE_LOG_GET_ARGS *args, uint32_t *flags,
      WT_ITEM *results_array, uint32_t *results_count)
    {
        auto acc = request(AccessMode::READ);
        Connection &conn = acc.conn;

        Connection::StatementPtr stmt = conn.db_statement(Statement::GET_PAGES);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, static_cast<sqlite3_int64>(table_id));
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 2, static_cast<sqlite3_int64>(page_id));
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 3, static_cast<sqlite3_int64>(args->lsn));
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 4, static_cast<sqlite3_int64>(now_us()));

        auto make_page_info = [](uint64_t table_id, uint64_t page_id, sqlite3_stmt *stmt) {
            PageInfo page{.table_id = table_id,
              .page_id = page_id,
              .lsn = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0)),
              .backlink_lsn = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1)),
              .base_lsn = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2)),
              .flags = static_cast<uint32_t>(sqlite3_column_int64(stmt, 3)),
              .encryption = WT_PAGE_LOG_ENCRYPTION{}};

            const char *enc = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
            strncpy(page.encryption.dek, enc ? enc : "", sizeof(page.encryption.dek));

            return std::move(page);
        };

        /*
         * Note: Results are ordered by lsn DESC, so the first row is the most recent.
         * In case of a delta chain, deltas (D) appear first, followed by the full page (B):
         *     D2, D1, B
         * We will reverse the order before returning to the caller:
         *     B, D1, D2
         */
        uint32_t count = 0;
        int ret = 0;
        std::vector<PageInfo> pages;

        while ((ret = SQ_CHECK(sqlite3_step, stmt.get())) == SQLITE_ROW && count < *results_count) {
            pages.push_back(make_page_info(table_id, page_id, stmt.get()));
            PageInfo &page = pages.back();

            if (page.flags & WT_PAGE_LOG_DISCARDED) {
                LOG_AND_THROW("Got discarded page: {}", page);
            }

            const void *blob = sqlite3_column_blob(stmt.get(), 5);
            int size = sqlite3_column_bytes(stmt.get(), 5);
            fill_item(&results_array[count], blob, static_cast<size_t>(size));

            /* Continue to next row */
            count++;

            if (!(page.flags & WT_PAGE_LOG_DELTA)) {
                /* Stop if this is a full page */
                ret = SQLITE_DONE;
                break;
            }
        }
        /* By now ret is either SQLITE_DONE or SQLITE_ROW. */

        /* SQLITE_ROW means there are more rows available than space in results_array. */
        if (ret == SQLITE_ROW) {
            LOG_AND_THROW("Insufficient space in results_array: {}", *results_count);
        }

        /* Verify full page, if configured */
        const uint64_t prev_full_lsn =
          config.verify ? get_prev_full_page_lsn(acc.conn, table_id, page_id, args->lsn) : 0;
        /* Always verify the delta chain when retrieving pages, even if config.verify=false. */
        verify_chain(pages, prev_full_lsn);

        /* Fill args from the first found page. (It will be the last in returned array.) */
        uint64_t save_lsn = args->lsn;
        memset(args, 0, sizeof(*args));
        args->lsn = save_lsn; /* Preserve the requested LSN */
        if (!pages.empty()) {
            const PageInfo &page = pages.front();
            args->backlink_lsn = page.backlink_lsn;
            args->base_lsn = page.base_lsn;
            args->encryption = page.encryption;
            assert(flags != nullptr);
            *flags = page.flags;
        }

        /* Reverse the results array to have the full page first, followed by deltas. */
        std::reverse(results_array, results_array + count);
        *results_count = count;
    }

    void
    get_ids(uint64_t checkpoint_lsn, uint64_t table_id, WT_ITEM *page_ids, size_t *page_count)
    {
        auto acc = request(AccessMode::READ);
        Connection &conn = acc.conn;

        Connection::StatementPtr stmt = conn.db_statement(Statement::GET_PAGE_IDS);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, static_cast<sqlite3_int64>(table_id));
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 2, static_cast<sqlite3_int64>(checkpoint_lsn));

        std::vector<uint64_t> ids;
        while (SQ_CHECK(sqlite3_step, stmt.get()) == SQLITE_ROW) {
            ids.push_back(sqlite3_column_int64(stmt.get(), 0));
        }
        LOG_DEBUG("Found {} page IDs for table_id={} at checkpoint_lsn={}", ids.size(), table_id,
          checkpoint_lsn);
        LOG_DIAG("Page IDs: {}", join(ids, ", "));

        *page_count = ids.size();
        if (ids.size() > 0) {
            fill_item(page_ids, ids.data(), ids.size() * sizeof(uint64_t));
        }
    }

    void
    discard(uint64_t table_id, uint64_t page_id, uint64_t lsn, WT_PAGE_LOG_DISCARD_ARGS *args)
    {
        if (args->flags != 0)
            LOG_AND_THROW("Non-zero flags: {:#x}", args->flags);

        WT_PAGE_LOG_PUT_ARGS put_args{};
        put_args.backlink_lsn = args->backlink_lsn;
        put_args.base_lsn = args->base_lsn;
        /* Discarded pages are deltas */
        put_args.flags = WT_PAGE_LOG_DELTA | WT_PAGE_LOG_DISCARDED;

        WT_ITEM dummy_page{};
        put(table_id, page_id, lsn, &put_args, &dummy_page);
        args->lsn = put_args.lsn;

        LOG_DEBUG("Discarded page_id={} at lsn={}, backlink_lsn={}, base_lsn={}; discarded_lsn={}",
          page_id, lsn, args->backlink_lsn, args->base_lsn, args->lsn);
    }

    void
    delete_many(uint64_t lsn, AccessMode mode = AccessMode::WRITE)
    {
        auto acc = request(mode);
        Connection &conn = acc.conn;

        LOG_DEBUG("Deleting pages with lsn > {}", lsn);
        Connection::StatementPtr stmt = conn.db_statement(Statement::DELETE_PAGES);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, static_cast<sqlite3_int64>(lsn));
        SQ_CHECK(sqlite3_step, stmt.get());
    }

private:
    uint64_t
    get_prev_full_page_lsn(Connection &conn, uint64_t table_id, uint64_t page_id, uint64_t lsn)
    {
        Connection::StatementPtr stmt = conn.db_statement(Statement::GET_FULL_PAGE_LSN);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, static_cast<sqlite3_int64>(table_id));
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 2, static_cast<sqlite3_int64>(page_id));
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 3, static_cast<sqlite3_int64>(lsn));

        int ret = SQ_CHECK(sqlite3_step, stmt.get());
        if (ret == SQLITE_DONE) {
            return 0; /* No previous full page found */
        }

        return static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0));
    }

    /* Invalid LSN means "no check required". */
    void
    verify_chain(const std::vector<PageInfo> &pages, uint64_t prev_full_lsn = INVALID_LSN)
    {
        PageInfo prev{};
        PageInfo full{};
        PageInfo discarded{};

        auto verify_full = [&](const PageInfo &page) {
            if (full.lsn != 0) {
                LOG_AND_THROW("Multiple full pages in chain: {}, {}", full, page);
            }

            if (page.base_lsn != 0) {
                LOG_AND_THROW("Full page base_lsn must be 0: {}", page);
            }

            if (prev_full_lsn != INVALID_LSN && page.backlink_lsn != prev_full_lsn) {
                LOG_AND_THROW(
                  "Full page backlink_lsn mismatch: {}, expected: {}", page, prev_full_lsn);
            }

            if (page.flags & WT_PAGE_LOG_DISCARDED) {
                LOG_AND_THROW("Full page cannot be discarded: {}", page);
            }

            if (discarded.lsn != 0) {
                LOG_AND_THROW(
                  "Discarded page cannot be followed by another page: {}, {}", discarded, page);
            }

            full = page;
        };

        auto verify_delta = [&](const PageInfo &page) {
            assert(prev.lsn != 0); /* Cannot be the first page in the chain */

            if (full.lsn == 0) {
                LOG_AND_THROW("Delta page without full page: {}", page);
            }

            if (full.lsn != page.base_lsn) {
                LOG_AND_THROW("Delta page base_lsn mismatch: {}, full page: {}", page, full);
            }

            /* Discarded pages can backlink to any page that we've seen in the chain. */
            if (!(page.flags & WT_PAGE_LOG_DISCARDED) && page.backlink_lsn != prev.lsn) {
                LOG_AND_THROW(
                  "Delta chain backlink_lsn mismatch: {}, previous page: {}", page, prev);
            }

            if (page.flags & WT_PAGE_LOG_DISCARDED) {
                if (discarded.lsn != 0) {
                    LOG_AND_THROW("Multiple discarded pages in chain: {}, {}", discarded, page);
                }
                discarded = page;
            } else if (discarded.lsn != 0) {
                LOG_AND_THROW(
                  "Discarded page cannot be followed by another page: {}, {}", discarded, page);
            }
        };

        auto verify_page = [&](const PageInfo &page) {
            if (page.flags & WT_PAGE_LOG_DELTA)
                verify_delta(page);
            else
                verify_full(page);
            prev = page;
        };

        /* Iterate in reverse order to validate the chain from full page. */
        std::for_each(pages.rbegin(), pages.rend(), verify_page);

        /* Verify discarded page separately because it can backlink to any page in the chain. */
        if (discarded.lsn != 0) {
            auto found_it = std::ranges::find_if(
              pages, [&discarded](const PageInfo &p) { return p.lsn == discarded.backlink_lsn; });
            if (found_it == pages.end()) {
                LOG_AND_THROW("Discarded page backlink_lsn not found in chain: {}", discarded);
            }
        }
    }

    void
    get_infos(Connection &conn, uint64_t table_id, uint64_t page_id, uint64_t lsn,
      std::vector<PageInfo> &pages)
    {
        Connection::StatementPtr stmt = conn.db_statement(Statement::GET_PAGE_INFOS);
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 1, static_cast<sqlite3_int64>(table_id));
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 2, static_cast<sqlite3_int64>(page_id));
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 3, static_cast<sqlite3_int64>(lsn));
        SQ_CHECK(sqlite3_bind_int64, stmt.get(), 4, static_cast<sqlite3_int64>(now_us()));

        pages.clear();
        while (SQ_CHECK(sqlite3_step, stmt.get()) == SQLITE_ROW) {
            pages.emplace_back(
              PageInfo{.table_id = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0)),
                .page_id = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 1)),
                .lsn = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 2)),
                .backlink_lsn = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 3)),
                .base_lsn = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 4)),
                .flags = static_cast<uint32_t>(sqlite3_column_int64(stmt.get(), 5)),
                .encryption = WT_PAGE_LOG_ENCRYPTION{}});
        }
    }

    /*
     * Verify the entire page chain for consistency.
     *
     * The pages are expected to be ordered by lsn DESC (newest first).
     * The chain may include a single discarded page as the first page,
     * zero or more deltas, and one terminating full page.
     * Discarded pages can backlink to any page in the chain.
     *
     * Example of a valid chain:
     *     Discarded (lsn=10, backlink_lsn=8, base_lsn=7, flags=DISCARDED|DELTA)
     *     Delta     (lsn=9,  backlink_lsn=8, base_lsn=7, flags=DELTA)
     *     Delta     (lsn=8,  backlink_lsn=7, base_lsn=7, flags=DELTA)
     *     Full      (lsn=7,  backlink_lsn=0, base_lsn=0, flags=0)
     *
     * Throws on error.
     */
    void
    verify_chain(Connection &conn, uint64_t table_id, uint64_t page_id, uint64_t lsn)
    {
        std::vector<PageInfo> pages;
        get_infos(conn, table_id, page_id, lsn, pages);
        if (pages.size() == 0) {
            LOG_AND_THROW(
              "No pages found for table_id={}, page_id={} at lsn<={}", table_id, page_id, lsn);
        }

        const uint64_t prev_full_lsn = get_prev_full_page_lsn(conn, table_id, page_id, lsn);

        verify_chain(pages, prev_full_lsn);
    }
};

class Storage {
    Config &config;
    const std::filesystem::path db_home;

    Globals globals;
    Checkpoints checkpoints;

    std::unordered_map<uint64_t, std::unique_ptr<Pages>> pages;
    std::shared_mutex pages_access; /* protects 'pages' map */

    /* Enables exclusive access to entire storage */
    std::shared_mutex store_access;

    /* Stats */
    std::atomic_ullong object_puts; /* (What would be) network writes */
    std::atomic_ullong object_gets; /* (What would be) network requests for data */

public:
    ~Storage() = default;
    Storage(Config &cfg, const std::filesystem::path &dbp)
        : config(cfg), db_home(dbp), globals(cfg, store_access, db_home),
          checkpoints(cfg, store_access, db_home)
    {
    }

    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

private:
    Pages &
    get_or_open_table(uint64_t table_id)
    {
        std::unique_lock write_lock(pages_access);
        auto [it, inserted] = pages.try_emplace(
          table_id, std::make_unique<Pages>(config, store_access, db_home, table_id));
        return *(it->second);
    }

    Pages &
    get_table(uint64_t table_id)
    {
        std::shared_lock read_lock(pages_access);
        auto it = pages.find(table_id);
        if (it == pages.end()) {
            LOG_AND_THROW("Pages table not found for table ID {}", table_id);
        }

        return *(it->second);
    }

public:
    void
    close()
    {
        std::unique_lock write_lock(store_access);
        std::ranges::for_each(pages, [](auto &kv) { kv.second->close(); });
        pages.clear();

        checkpoints.close();
        globals.close();
    }

    /* Artificial delay or simulated network error during an object transfer. */
    void
    simulate_unstable_network()
    {
        const auto ops = object_gets + object_puts;
        auto simulate = [this, ops](uint32_t every, uint32_t avg_ms, bool is_error) {
            if (!every || ops % every)
                return;

            const uint64_t msec = jitter_ms(avg_ms);
            LOG_TRACE("Artificial {} {} ms after {} object reads, {} object writes",
              is_error ? "error (sleep)" : "delay", msec, object_gets.load(), object_puts.load());
            std::this_thread::sleep_for(std::chrono::milliseconds(msec));

            if (is_error) {
                throw std::system_error(
                  std::make_error_code(std::errc::network_unreachable), "Simulated network error");
            }
        };

        simulate(config.force_delay, config.delay_ms, false);
        simulate(config.force_error, config.error_ms, true);
    }

    uint64_t
    make_next_lsn()
    {
        return globals.make_next_lsn();
    }

    uint64_t
    get_last_lsn()
    {
        return globals.get_last_lsn();
    }

    void
    add_table_id(uint64_t table_id)
    {
        /* Ensure Pages table exists for this table ID */
        get_or_open_table(table_id);

        /*
         * Add table ID to global table. Adding more than once is OK. The table index will ensure
         * uniqueness.
         */
        globals.add_table_id(table_id);
    }

    std::vector<uint64_t>
    get_table_ids()
    {
        return globals.get_table_ids();
    }

    void
    put_checkpoint(uint64_t lsn, uint64_t checkpoint_timestamp, const WT_ITEM *checkpoint_metadata)
    {
        checkpoints.put(lsn, checkpoint_timestamp, checkpoint_metadata);
    }

    int
    get_checkpoint(uint64_t &lsn, uint64_t *timestamp, WT_ITEM *checkpoint_metadata)
    {
        return checkpoints.get(lsn, timestamp, checkpoint_metadata);
    }

    void
    put_page(uint64_t table_id, uint64_t page_id, uint64_t lsn, WT_PAGE_LOG_PUT_ARGS *args,
      const WT_ITEM *buf)
    {
        Pages &table = get_table(table_id);
        table.put(table_id, page_id, lsn, args, buf);
        object_puts++;
    }

    void
    get_pages(uint64_t table_id, uint64_t page_id, WT_PAGE_LOG_GET_ARGS *args, uint32_t *flags,
      WT_ITEM *results_array, uint32_t *results_count)
    {
        Pages &table = get_table(table_id);
        table.get(table_id, page_id, args, flags, results_array, results_count);

        assert(results_count != nullptr);
        object_gets += *results_count;
    }

    void
    get_page_ids(uint64_t checkpoint_lsn, uint64_t table_id, WT_ITEM *page_ids, size_t *page_count)
    {
        Pages &table = get_table(table_id);
        table.get_ids(checkpoint_lsn, table_id, page_ids, page_count);
    }

    void
    discard_page(uint64_t table_id, uint64_t page_id, uint64_t lsn, WT_PAGE_LOG_DISCARD_ARGS *args)
    {
        Pages &table = get_table(table_id);
        table.discard(table_id, page_id, lsn, args);
    }

    void
    abandon_checkpoint(uint64_t checkpoint_lsn)
    {
        /* Ensure exclusive access to storage since we update multiple tables. */
        std::unique_lock write_lock(store_access);

        int ret = 0;
        if (checkpoint_lsn == WT_PAGE_LOG_LSN_MAX) {
            ret =
              checkpoints.get(checkpoint_lsn, nullptr, nullptr, Checkpoints::AccessMode::BYPASS);
        }

        if (ret == WT_NOTFOUND) {
            LOG_DEBUG("No checkpoint found to abandon; lsn = {}", checkpoint_lsn);
            return;
        }

        /* Note: global LSN counter is not decremented */

        checkpoints.delete_many(checkpoint_lsn, Checkpoints::AccessMode::BYPASS);

        for (auto table_id : globals.get_table_ids(Globals::AccessMode::BYPASS)) {
            /*
             * Table ID's may be recorded in the 'globals' table, however connection to their
             * 'pages' table may have never been opened. For example, on restart. Therefore, we use
             * get_or_open_table().
             */
            auto &pages_tbl = get_or_open_table(table_id);
            pages_tbl.delete_many(checkpoint_lsn, Pages::AccessMode::BYPASS);
        }
    }
};

/*
 * PaliteHandle class
 */

class PaliteHandle : public WT_PAGE_LOG_HANDLE {
    uint64_t table_id; /* Table ID for this handle */
    Storage &storage;

    void initialize_interface();

public:
    Config &config;

    ~PaliteHandle() = default;
    PaliteHandle(WT_PAGE_LOG *palite, Config &cfg, Storage &store, uint64_t tid)
        : WT_PAGE_LOG_HANDLE{}, table_id(tid), config(cfg), storage(store)
    {
        WT_PAGE_LOG_HANDLE::page_log = palite;
        initialize_interface();
        storage.add_table_id(table_id);
        LOG_DEBUG("Created PaliteHandle for table_id={}", table_id);
    }

    /* Instance methods that implement the actual functionality */
    int
    put(uint64_t page_id, uint64_t checkpoint_id, WT_PAGE_LOG_PUT_ARGS *args, const WT_ITEM *buf)
    {
        storage.simulate_unstable_network();

        /* TODO: handle dek encryption */

        const uint64_t lsn = storage.make_next_lsn();
        storage.put_page(table_id, page_id, lsn, args, buf);
        args->lsn = lsn;

        LOG_DIAG(
          "Put page_id={} at lsn={}, backlink_lsn={}, base_lsn={}, "
          "flags={:#x}, image_size={}",
          page_id, lsn, args->backlink_lsn, args->base_lsn, args->flags, args->image_size);
        LOG_TRACE("Page data (size={}) =====\n{}", buf->size, verbose_item(buf));

        return 0;
    }

    int
    get(uint64_t page_id, uint64_t checkpoint_id, WT_PAGE_LOG_GET_ARGS *args,
      WT_ITEM *results_array, uint32_t *results_count)
    {
        storage.simulate_unstable_network();

        uint32_t flags = 0;
        storage.get_pages(table_id, page_id, args, &flags, results_array, results_count);

        LOG_DEBUG(
          "Get page_id={} at lsn={}, returned {} entries, "
          "backlink_lsn={}, base_lsn={}, flags={:#x}",
          page_id, args->lsn, *results_count, args->backlink_lsn, args->base_lsn, flags);

        return 0;
    }

    int
    get_page_ids(uint64_t checkpoint_lsn, WT_ITEM *page_ids, size_t *page_count)
    {
        storage.get_page_ids(checkpoint_lsn, table_id, page_ids, page_count);

        LOG_DEBUG(
          "Get page_ids for table_id={} at checkpoint_lsn={}, "
          "returned {} page IDs",
          table_id, checkpoint_lsn, *page_count);

        return 0;
    }

    int
    discard(uint64_t page_id, uint64_t checkpoint_id, WT_PAGE_LOG_DISCARD_ARGS *args)
    {
        storage.simulate_unstable_network();

        const uint64_t lsn = storage.make_next_lsn();
        storage.discard_page(table_id, page_id, lsn, args);
        args->lsn = lsn;

        LOG_DEBUG("Discard page_id={} at lsn={}, backlink_lsn={}, base_lsn={}", page_id, lsn,
          args->backlink_lsn, args->base_lsn);

        return 0;
    }

    int
    close()
    {
        LOG_DEBUG("Closing PaliteHandle for table_id={}", table_id);
        delete this; /* No calls expected after WT_PAGE_LOG_HANDLE::plh_close */
        return 0;
    }
};

extern "C" {
static int
palite_handle_put(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *sess, uint64_t page_id,
  uint64_t checkpoint_id, WT_PAGE_LOG_PUT_ARGS *args, const WT_ITEM *buf)
{
    return safe_call<PaliteHandle>(
      sess, plh, &PaliteHandle::put, page_id, checkpoint_id, args, buf);
}

static int
palite_handle_get(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *sess, uint64_t page_id,
  uint64_t checkpoint_id, WT_PAGE_LOG_GET_ARGS *args, WT_ITEM *results_array,
  uint32_t *results_count)
{
    return safe_call<PaliteHandle>(
      sess, plh, &PaliteHandle::get, page_id, checkpoint_id, args, results_array, results_count);
}

static int
palite_handle_get_page_ids(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *sess, uint64_t checkpoint_lsn,
  WT_ITEM *page_ids, size_t *page_count)
{
    return safe_call<PaliteHandle>(
      sess, plh, &PaliteHandle::get_page_ids, checkpoint_lsn, page_ids, page_count);
}

static int
palite_handle_discard(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *sess, uint64_t page_id,
  uint64_t checkpoint_id, WT_PAGE_LOG_DISCARD_ARGS *args)
{
    return safe_call<PaliteHandle>(sess, plh, &PaliteHandle::discard, page_id, checkpoint_id, args);
}

static int
palite_handle_close(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *sess)
{
    return safe_call<PaliteHandle>(sess, plh, &PaliteHandle::close);
}
} /* extern "C" */

void
PaliteHandle::initialize_interface()
{
    plh_put = palite_handle_put;
    plh_get = palite_handle_get;
    plh_get_page_ids = palite_handle_get_page_ids;
    plh_discard = palite_handle_discard;
    plh_close = palite_handle_close;
}

/*
 * Palite class
 */
class Palite : public WT_PAGE_LOG {
public:
    uint64_t begin_lsn;        /* LSN at creation time */
    std::atomic_int ref_count; /* Reference counting for the page log service */
    Config config;             /* Configuration options */
    Storage storage;           /* Storage backend for page log */

public:
    ~Palite() = default;
    Palite(const std::filesystem::path &home_dir, WT_EXTENSION_API *wt_api, WT_CONFIG_ARG *cfg_arg)
        : WT_PAGE_LOG(), ref_count(1), config(wt_api, cfg_arg),
          storage(config, initialize_directory(home_dir))
    {
        LOG_DEBUG("Initializing Palite page log extension, config: {}", config);
        initialize_interface();
        get_last_lsn(&begin_lsn);
        LOG_DEBUG("Created Palite page log at '{}', ref_count={}, begin_lsn={}", home_dir.string(),
          ref_count.load(), begin_lsn);
    }

    std::filesystem::path
    initialize_directory(const std::filesystem::path &home_dir)
    {
        std::filesystem::path kv_home =
          (!config.home_dir.empty() ? config.home_dir : home_dir) / "kv_home";
        if (!std::filesystem::exists(kv_home)) {
            std::filesystem::create_directories(kv_home);
            LOG_DEBUG("Created directory for Palite page log: {}", kv_home.string());
        } else {
            LOG_DEBUG("Using existing directory for Palite page log: {}", kv_home.string());
        }
        return kv_home;
    }

    void initialize_interface();

    int
    add_reference()
    {
        ++ref_count;
        LOG_DEBUG("Adding reference to page log, new ref_count={}", ref_count.load());
        return 0;
    }

    int
    begin_checkpoint(uint64_t checkpoint_id)
    {
        LOG_DEBUG("checkpoint_id={}", checkpoint_id);
        return 0;
    }

    /*
     * Abandon (delete) all page log entries and checkpoint records with an LSN greater than the
     * given LSN.
     */
    int
    abandon_checkpoint(uint64_t checkpoint_lsn)
    {
        LOG_DEBUG("Abandoning checkpoint at lsn={}", checkpoint_lsn);
        storage.abandon_checkpoint(checkpoint_lsn);

        return 0;
    }

    int
    complete_checkpoint_ext(uint64_t checkpoint_id, uint64_t checkpoint_timestamp,
      const WT_ITEM *checkpoint_metadata, uint64_t *lsnp)
    {
        const uint64_t lsn = storage.make_next_lsn();
        storage.put_checkpoint(lsn, checkpoint_timestamp, checkpoint_metadata);

        LOG_DEBUG(
          "checkpoint_id={}, timestamp={}, lsn={}", checkpoint_id, checkpoint_timestamp, lsn);
        LOG_TRACE("checkpoint_metadata (size={}) =====\n{}",
          checkpoint_metadata ? checkpoint_metadata->size : 0,
          checkpoint_metadata ? verbose_item(checkpoint_metadata) : "<none>");

        if (lsnp) {
            *lsnp = lsn;
        }

        return 0;
    }

    int
    get_complete_checkpoint_ext(uint64_t *checkpoint_lsn, uint64_t *checkpoint_id,
      uint64_t *checkpoint_timestamp, WT_ITEM *checkpoint_metadata)
    {
        if (checkpoint_lsn)
            *checkpoint_lsn = 0;
        if (checkpoint_id)
            *checkpoint_id = 0;
        if (checkpoint_timestamp)
            *checkpoint_timestamp = 0;

        uint64_t last_ckpt_lsn = WT_PAGE_LOG_LSN_MAX; /* most recent checkpoint */
        int ret = storage.get_checkpoint(last_ckpt_lsn, checkpoint_timestamp, checkpoint_metadata);

        LOG_DEBUG("checkpoint_lsn={}, timestamp={}", last_ckpt_lsn,
          checkpoint_timestamp ? *checkpoint_timestamp : 0);
        LOG_TRACE("checkpoint_metadata (size={}) =====\n{}",
          checkpoint_metadata ? checkpoint_metadata->size : 0,
          checkpoint_metadata ? verbose_item(checkpoint_metadata) : "<none>");

        if (checkpoint_lsn)
            *checkpoint_lsn = last_ckpt_lsn;

        return ret;
    }

    int
    get_last_lsn(uint64_t *lsn)
    {
        if (!lsn) {
            LOG_WARN("Invalid argument: lsn is null");
            return EINVAL;
        }

        *lsn = storage.get_last_lsn();
        LOG_TRACE("last_lsn={}", *lsn);

        return 0;
    }

    int
    open_handle(uint64_t table_id, WT_PAGE_LOG_HANDLE **plh)
    {
        if (!plh) {
            LOG_WARN("Invalid argument: plh is null");
            return EINVAL;
        }

        PaliteHandle *handle = new PaliteHandle(this, config, storage, table_id);
        *plh = static_cast<WT_PAGE_LOG_HANDLE *>(handle);
        LOG_DEBUG("Opened handle for table_id={}", table_id);

        return 0;
    }

    int
    set_last_materialized_lsn(uint64_t lsn)
    {
        config.last_materialized_lsn = lsn; /* Update config value */
        LOG_DEBUG("Set last_materialized_lsn={}", lsn);
        return 0;
    }

    int
    terminate()
    {
        --ref_count;
        LOG_DEBUG("Terminating page log, new ref_count={}", ref_count.load());
        if (ref_count <= 0) {
            storage.close();
            LOG_DEBUG("Destroying Palite page log");
            delete this; /* No calls expected after the last WT_PAGE_LOG::terminate */
        }
        return 0;
    }
};

extern "C" {
static int
palite_add_reference(WT_PAGE_LOG *page_log)
{
    return safe_call<Palite>(nullptr, page_log, &Palite::add_reference);
}

static int
palite_abandon_checkpoint(WT_PAGE_LOG *page_log, WT_SESSION *sess, uint64_t last_checkpoint_lsn)
{
    return safe_call<Palite>(sess, page_log, &Palite::abandon_checkpoint, last_checkpoint_lsn);
}

static int
palite_begin_checkpoint(WT_PAGE_LOG *page_log, WT_SESSION *sess, uint64_t checkpoint_id)
{
    return safe_call<Palite>(sess, page_log, &Palite::begin_checkpoint, checkpoint_id);
}

static int
palite_complete_checkpoint_ext(WT_PAGE_LOG *page_log, WT_SESSION *sess, uint64_t checkpoint_id,
  uint64_t checkpoint_timestamp, const WT_ITEM *checkpoint_metadata, uint64_t *lsnp)
{
    return safe_call<Palite>(sess, page_log, &Palite::complete_checkpoint_ext, checkpoint_id,
      checkpoint_timestamp, checkpoint_metadata, lsnp);
}

static int
palite_get_complete_checkpoint_ext(WT_PAGE_LOG *page_log, WT_SESSION *sess,
  uint64_t *checkpoint_lsn, uint64_t *checkpoint_id, uint64_t *checkpoint_timestamp,
  WT_ITEM *checkpoint_metadata)
{
    return safe_call<Palite>(sess, page_log, &Palite::get_complete_checkpoint_ext, checkpoint_lsn,
      checkpoint_id, checkpoint_timestamp, checkpoint_metadata);
}

static int
palite_get_last_lsn(WT_PAGE_LOG *page_log, WT_SESSION *sess, uint64_t *lsn)
{
    return safe_call<Palite>(sess, page_log, &Palite::get_last_lsn, lsn);
}

static int
palite_open_handle(
  WT_PAGE_LOG *page_log, WT_SESSION *sess, uint64_t table_id, WT_PAGE_LOG_HANDLE **plh)
{
    return safe_call<Palite>(sess, page_log, &Palite::open_handle, table_id, plh);
}

static int
palite_set_last_materialized_lsn(WT_PAGE_LOG *page_log, WT_SESSION *sess, uint64_t lsn)
{
    return safe_call<Palite>(sess, page_log, &Palite::set_last_materialized_lsn, lsn);
}

static int
palite_terminate(WT_PAGE_LOG *page_log, WT_SESSION *sess)
{
    return safe_call<Palite>(sess, page_log, &Palite::terminate);
}
} /* extern "C" */

void
Palite::initialize_interface()
{
    pl_add_reference = palite_add_reference;
    pl_abandon_checkpoint = palite_abandon_checkpoint;
    pl_begin_checkpoint = palite_begin_checkpoint;
    pl_complete_checkpoint_ext = palite_complete_checkpoint_ext;
    pl_get_complete_checkpoint_ext = palite_get_complete_checkpoint_ext;
    pl_get_last_lsn = palite_get_last_lsn;
    pl_open_handle = palite_open_handle;
    pl_set_last_materialized_lsn = palite_set_last_materialized_lsn;
    static_cast<WT_PAGE_LOG *>(this)->terminate = palite_terminate;
}

/*
 * palite_extension_init --
 *     A standalone, durable implementation of the WT_PAGE_LOG interface (PALI).
 */
extern "C" {
static int
palite_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *cfg_arg)
{
    int ret = 0;
    session(nullptr);
    Config config{}; /* default config for logging macros bellow */

    try {
        const auto home_dir = connection->get_home(connection);
        auto wt_api = connection->get_extension_api(connection);
        std::unique_ptr<Palite> palite = std::make_unique<Palite>(home_dir, wt_api, cfg_arg);

        /* Register the page log service with WiredTiger */
        ret = connection->add_page_log(connection, "palite", palite.get(), nullptr);

        if (ret == 0) {
            palite.release();
        }
    } catch (const std::bad_alloc &e) {
        LOG_ERROR("Memory allocation failed: {}", e.what());
        ret = ENOMEM;
    } catch (const std::invalid_argument &e) {
        LOG_ERROR("Invalid argument: {}", e.what());
        ret = EINVAL;
    } catch (const std::filesystem::filesystem_error &e) {
        LOG_ERROR("Filesystem error: {}", e.what());
        ret = EIO;
    } catch (const std::runtime_error &e) {
        LOG_ERROR("Runtime error: {}", e.what());
        ret = EINVAL;
    } catch (const std::exception &e) {
        LOG_ERROR("Exception: {}", e.what());
        ret = EINVAL;
    } catch (...) {
        LOG_ERROR("Unknown error occurred");
        ret = EFAULT;
    }

    return ret;
}
} /* extern "C" */

/*
 * We have to remove this symbol when building as a builtin extension otherwise it will conflict
 * with other builtin libraries.
 */
#ifndef HAVE_BUILTIN_EXTENSION_PALITE

/*
 * wiredtiger_extension_init --
 *     WiredTiger page and log mock extension.
 */
extern "C" int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    return palite_extension_init(connection, config);
}
#endif
