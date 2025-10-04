// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_file_client.h"

#if defined(HAVE_GSL)
#  include <gsl/gsl>
#else
#  include <assert.h>
#endif

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h" // IWYU pragma: keep
// clang-format on

#include "google/protobuf/message.h"
#include "nlohmann/json.hpp"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h" // IWYU pragma: keep
// clang-format on

#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/base64.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/version.h"

#ifdef _MSC_VER
#  include <string.h>
#  define strcasecmp _stricmp
#else
#  include <strings.h>
#endif

#include <limits.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if OPENTELEMETRY_HAVE_EXCEPTIONS
#  include <exception>
#endif

#if !defined(__CYGWIN__) && defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif

#  include <Windows.h>
#  include <direct.h>
#  include <io.h>

#  ifdef UNICODE
#    include <atlconv.h>
#    define VC_TEXT(x) A2W(x)
#  else
#    define VC_TEXT(x) x
#  endif

#  define FS_ACCESS(x) _access(x, 0)
#  define SAFE_STRTOK_S(...) strtok_s(__VA_ARGS__)
#  define FS_MKDIR(path, mode) _mkdir(path)

#else

#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>

#  define FS_ACCESS(x) access(x, F_OK)
#  define SAFE_STRTOK_S(...) strtok_r(__VA_ARGS__)
#  define FS_MKDIR(path, mode) ::mkdir(path, mode)

#  if defined(__ANDROID__)
#    define FS_DISABLE_LINK 1
#  elif defined(__APPLE__)
#    if __dest_os != __mac_os_x
#      define FS_DISABLE_LINK 1
#    endif
#  endif

#endif

#ifdef GetMessage
#  undef GetMessage
#endif

#if (defined(_MSC_VER) && _MSC_VER >= 1600) || \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__STDC_LIB_EXT1__)
#  ifdef _MSC_VER
#    define OTLP_FILE_SNPRINTF(buffer, bufsz, ...) \
      sprintf_s(buffer, static_cast<size_t>(bufsz), __VA_ARGS__)
#  else
#    define OTLP_FILE_SNPRINTF(buffer, bufsz, fmt, args...) \
      snprintf_s(buffer, static_cast<rsize_t>(bufsz), fmt, ##args)
#  endif
#else
#  define OTLP_FILE_SNPRINTF(buffer, bufsz, fmt, args...) \
    snprintf(buffer, static_cast<size_t>(bufsz), fmt, ##args)
#endif

#if (defined(_MSC_VER) && _MSC_VER >= 1600) || \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
#  define OTLP_FILE_OPEN(f, path, mode) fopen_s(&f, path, mode)
#else
#  include <errno.h>
#  define OTLP_FILE_OPEN(f, path, mode) f = fopen(path, mode)
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

namespace
{
static std::tm GetLocalTime()
{
  std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__STDC_LIB_EXT1__)
  std::tm ret;
  localtime_s(&now, &ret);
#elif defined(_MSC_VER) && _MSC_VER >= 1300
  std::tm ret;
  localtime_s(&ret, &now);
#elif defined(_XOPEN_SOURCE) || defined(_BSD_SOURCE) || defined(_SVID_SOURCE) || \
    defined(_POSIX_SOURCE)
  std::tm ret;
  localtime_r(&now, &ret);
#else
  std::tm ret = *localtime(&now);
#endif
  return ret;
}

static std::size_t FormatPath(char *buff,
                              size_t bufz,
                              opentelemetry::nostd::string_view fmt,
                              std::size_t rotate_index)
{
  if (nullptr == buff || 0 == bufz)
  {
    return 0;
  }

  if (fmt.empty())
  {
    buff[0] = '\0';
    return 0;
  }

  bool need_parse = false;
  bool running    = true;
  std::size_t ret = 0;
  std::tm tm_obj_cache;
  std::tm *tm_obj_ptr = nullptr;

#define LOG_FMT_FN_TM_MEM(VAR, EXPRESS) \
                                        \
  int VAR;                              \
                                        \
  if (nullptr == tm_obj_ptr)            \
  {                                     \
    tm_obj_cache = GetLocalTime();      \
    tm_obj_ptr   = &tm_obj_cache;       \
    (VAR)        = tm_obj_ptr->EXPRESS; \
  }                                     \
  else                                  \
  {                                     \
    (VAR) = tm_obj_ptr->EXPRESS;        \
  }

  for (size_t i = 0; i < fmt.size() && ret < bufz && running; ++i)
  {
    if (!need_parse)
    {
      if ('%' == fmt[i])
      {
        need_parse = true;
      }
      else
      {
        buff[ret++] = fmt[i];
      }
      continue;
    }

    need_parse = false;
    switch (fmt[i])
    {
      // =================== datetime ===================
      case 'Y': {
        if (bufz - ret < 4)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(year, tm_year + 1900);
          buff[ret++] = static_cast<char>(year / 1000 + '0');
          buff[ret++] = static_cast<char>((year / 100) % 10 + '0');
          buff[ret++] = static_cast<char>((year / 10) % 10 + '0');
          buff[ret++] = static_cast<char>(year % 10 + '0');
        }
        break;
      }
      case 'y': {
        if (bufz - ret < 2)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(year, tm_year + 1900);
          buff[ret++] = static_cast<char>((year / 10) % 10 + '0');
          buff[ret++] = static_cast<char>(year % 10 + '0');
        }
        break;
      }
      case 'm': {
        if (bufz - ret < 2)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(mon, tm_mon + 1);
          buff[ret++] = static_cast<char>(mon / 10 + '0');
          buff[ret++] = static_cast<char>(mon % 10 + '0');
        }
        break;
      }
      case 'j': {
        if (bufz - ret < 3)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(yday, tm_yday);
          buff[ret++] = static_cast<char>(yday / 100 + '0');
          buff[ret++] = static_cast<char>((yday / 10) % 10 + '0');
          buff[ret++] = static_cast<char>(yday % 10 + '0');
        }
        break;
      }
      case 'd': {
        if (bufz - ret < 2)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(mday, tm_mday);
          buff[ret++] = static_cast<char>(mday / 10 + '0');
          buff[ret++] = static_cast<char>(mday % 10 + '0');
        }
        break;
      }
      case 'w': {
        LOG_FMT_FN_TM_MEM(wday, tm_wday);
        buff[ret++] = static_cast<char>(wday + '0');
        break;
      }
      case 'H': {
        if (bufz - ret < 2)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(hour, tm_hour);
          buff[ret++] = static_cast<char>(hour / 10 + '0');
          buff[ret++] = static_cast<char>(hour % 10 + '0');
        }
        break;
      }
      case 'I': {
        if (bufz - ret < 2)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(hour, tm_hour % 12 + 1);
          buff[ret++] = static_cast<char>(hour / 10 + '0');
          buff[ret++] = static_cast<char>(hour % 10 + '0');
        }
        break;
      }
      case 'M': {
        if (bufz - ret < 2)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(minite, tm_min);
          buff[ret++] = static_cast<char>(minite / 10 + '0');
          buff[ret++] = static_cast<char>(minite % 10 + '0');
        }
        break;
      }
      case 'S': {
        if (bufz - ret < 2)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(sec, tm_sec);
          buff[ret++] = static_cast<char>(sec / 10 + '0');
          buff[ret++] = static_cast<char>(sec % 10 + '0');
        }
        break;
      }
      case 'F': {
        if (bufz - ret < 10)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(year, tm_year + 1900);
          LOG_FMT_FN_TM_MEM(mon, tm_mon + 1);
          LOG_FMT_FN_TM_MEM(mday, tm_mday);
          buff[ret++] = static_cast<char>(year / 1000 + '0');
          buff[ret++] = static_cast<char>((year / 100) % 10 + '0');
          buff[ret++] = static_cast<char>((year / 10) % 10 + '0');
          buff[ret++] = static_cast<char>(year % 10 + '0');
          buff[ret++] = '-';
          buff[ret++] = static_cast<char>(mon / 10 + '0');
          buff[ret++] = static_cast<char>(mon % 10 + '0');
          buff[ret++] = '-';
          buff[ret++] = static_cast<char>(mday / 10 + '0');
          buff[ret++] = static_cast<char>(mday % 10 + '0');
        }
        break;
      }
      case 'T': {
        if (bufz - ret < 8)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(hour, tm_hour);
          LOG_FMT_FN_TM_MEM(minite, tm_min);
          LOG_FMT_FN_TM_MEM(sec, tm_sec);
          buff[ret++] = static_cast<char>(hour / 10 + '0');
          buff[ret++] = static_cast<char>(hour % 10 + '0');
          buff[ret++] = ':';
          buff[ret++] = static_cast<char>(minite / 10 + '0');
          buff[ret++] = static_cast<char>(minite % 10 + '0');
          buff[ret++] = ':';
          buff[ret++] = static_cast<char>(sec / 10 + '0');
          buff[ret++] = static_cast<char>(sec % 10 + '0');
        }
        break;
      }
      case 'R': {
        if (bufz - ret < 5)
        {
          running = false;
        }
        else
        {
          LOG_FMT_FN_TM_MEM(hour, tm_hour);
          LOG_FMT_FN_TM_MEM(minite, tm_min);
          buff[ret++] = static_cast<char>(hour / 10 + '0');
          buff[ret++] = static_cast<char>(hour % 10 + '0');
          buff[ret++] = ':';
          buff[ret++] = static_cast<char>(minite / 10 + '0');
          buff[ret++] = static_cast<char>(minite % 10 + '0');
        }
        break;
      }

      // =================== rotate index ===================
      case 'n':
      case 'N': {
        std::size_t value = fmt[i] == 'n' ? rotate_index + 1 : rotate_index;
#if (defined(_MSC_VER) && _MSC_VER >= 1600) || \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__STDC_LIB_EXT1__)
#  ifdef _MSC_VER
        auto res =
            sprintf_s(&buff[ret], bufz - ret, "%llu", static_cast<unsigned long long>(value));
#  else
        auto res =
            snprintf_s(&buff[ret], bufz - ret, "%llu", static_cast<unsigned long long>(value));
#  endif
#else
        auto res = snprintf(&buff[ret], bufz - ret, "%llu", static_cast<unsigned long long>(value));
#endif
        if (res < 0)
        {
          running = false;
        }
        else
        {
          ret += static_cast<std::size_t>(res);
        }
        break;
      }

      // =================== unknown ===================
      default: {
        buff[ret++] = fmt[i];
        break;
      }
    }
  }

#undef LOG_FMT_FN_TM_MEM

  if (ret < bufz)
  {
    buff[ret] = '\0';
  }
  else
  {
    buff[bufz - 1] = '\0';
  }
  return ret;
}

class OPENTELEMETRY_LOCAL_SYMBOL FileSystemUtil
{
public:
  // When LongPathsEnabled on Windows, it allow 32767 characters in a absolute path.But it still
  // only allow 260 characters in a relative path. See
  // https://docs.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation

  static constexpr const std::size_t kMaxPathSize =
#if defined(MAX_PATH)
      MAX_PATH;
#elif defined(_MAX_PATH)
      _MAX_PATH;
#elif defined(PATH_MAX)
      PATH_MAX;
#else
      260;
#endif

  static constexpr const char kDirectorySeparator =
#if !defined(__CYGWIN__) && defined(_WIN32)
      '\\';
#else
      '/';
#endif

  static std::size_t GetFileSize(const char *file_path)
  {
    std::fstream file;
    file.open(file_path, std::ios::binary | std::ios::in);
    if (!file.is_open())
    {
      return 0;
    }

    file.seekg(std::ios::end);
    auto size = file.tellg();
    file.close();

    if (size > 0)
    {
      return static_cast<std::size_t>(size);
    }
    else
    {
      return 0;
    }
  }

  static std::string DirName(opentelemetry::nostd::string_view file_path, int depth = 1)
  {
    if (file_path.empty())
    {
      return "";
    }

    std::size_t sz = file_path.size() - 1;

    while (sz > 0 && ('/' == file_path[sz] || '\\' == file_path[sz]))
    {
      --sz;
    }

    while (sz > 0 && depth > 0)
    {
      if ('/' == file_path[sz] || '\\' == file_path[sz])
      {
        // DirName(a//\b) -> a
        while (sz > 0 && ('/' == file_path[sz] || '\\' == file_path[sz]))
        {
          --sz;
        }

        --depth;
        if (depth <= 0)
        {
          ++sz;
          break;
        }
      }
      else
      {
        --sz;
      }
    }

    return static_cast<std::string>(file_path.substr(0, sz));
  }

  static bool IsExist(const char *file_path) { return 0 == FS_ACCESS(file_path); }

  static std::vector<std::string> SplitPath(opentelemetry::nostd::string_view path,
                                            bool normalize = false)
  {
    std::vector<std::string> out;

    std::string path_buffer = static_cast<std::string>(path);

    char *saveptr = nullptr;
    char *token   = SAFE_STRTOK_S(&path_buffer[0], "\\/", &saveptr);
    while (nullptr != token)
    {
      if (0 != strlen(token))
      {
        if (normalize)
        {
          // Normalize path
          if (0 == strcmp("..", token))
          {
            if (!out.empty() && out.back() != "..")
            {
              out.pop_back();
            }
            else
            {
              out.push_back(token);
            }
          }
          else if (0 != strcmp(".", token))
          {
            out.push_back(token);
          }
        }
        else
        {
          out.push_back(token);
        }
      }
      token = SAFE_STRTOK_S(nullptr, "\\/", &saveptr);
    }

    return out;
  }

  static bool MkDir(const char *dir_path, bool recursion, OPENTELEMETRY_MAYBE_UNUSED int mode)
  {
#if !(!defined(__CYGWIN__) && defined(_WIN32))
    if (0 == mode)
    {
      mode = S_IRWXU | S_IRWXG | S_IRWXO;
    }
#endif
    if (!recursion)
    {
      return 0 == FS_MKDIR(dir_path, static_cast<mode_t>(mode));
    }

    std::vector<std::string> path_segs = SplitPath(dir_path, true);

    if (path_segs.empty())
    {
      return false;
    }

    std::string current_path;
    if (nullptr != dir_path && ('/' == *dir_path || '\\' == *dir_path))
    {
      current_path.reserve(strlen(dir_path) + 4);
      current_path = *dir_path;

      // NFS Supporting
      char next_char = *(dir_path + 1);
      if ('/' == next_char || '\\' == next_char)
      {
        current_path += next_char;
      }
    }

    for (size_t i = 0; i < path_segs.size(); ++i)
    {
      current_path += path_segs[i];

      if (false == IsExist(current_path.c_str()))
      {
        if (0 != FS_MKDIR(current_path.c_str(), static_cast<mode_t>(mode)))
        {
          return false;
        }
      }

      current_path += kDirectorySeparator;
    }

    return true;
  }

#if !defined(UTIL_FS_DISABLE_LINK)
  enum class LinkOption : uint8_t
  {
    kDefault       = 0x00,  // hard link for default
    kSymbolicLink  = 0x01,  // or soft link
    kDirectoryLink = 0x02,  // it's used only for windows
    kForceRewrite  = 0x04,  // delete the old file if it exists
  };

  /**
   * @brief Create link
   * @param oldpath source path
   * @param newpath target path
   * @param options options
   * @return 0 for success, or error code
   */
  static int Link(const char *oldpath,
                  const char *newpath,
                  int32_t options = static_cast<int32_t>(LinkOption::kDefault))
  {
    if ((options & static_cast<int32_t>(LinkOption::kForceRewrite)) && IsExist(newpath))
    {
      remove(newpath);
    }

#  if !defined(__CYGWIN__) && defined(_WIN32)
#    if defined(UNICODE)
    USES_CONVERSION;
#    endif

    if (options & static_cast<int32_t>(LinkOption::kSymbolicLink))
    {
      DWORD dwFlags = 0;
      if (options & static_cast<int32_t>(LinkOption::kDirectoryLink))
      {
        dwFlags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
#    if defined(SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
        dwFlags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#    endif
      }

      if (CreateSymbolicLink(VC_TEXT(newpath), VC_TEXT(oldpath), dwFlags))
      {
        return 0;
      }

      return static_cast<int>(GetLastError());
    }
    else
    {
      if (CreateHardLink(VC_TEXT(newpath), VC_TEXT(oldpath), nullptr))
      {
        return 0;
      }

      return static_cast<int>(GetLastError());
    }

#  else
    int opts = 0;
    if (options & static_cast<int32_t>(LinkOption::kSymbolicLink))
    {
      opts = AT_SYMLINK_FOLLOW;
    }

    int res = ::linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, opts);
    if (0 == res)
    {
      return 0;
    }

    return errno;

#  endif
  }
#endif
};

static inline char HexEncode(unsigned char byte)
{
#if defined(HAVE_GSL)
  Expects(byte <= 16);
#else
  assert(byte <= 16);
#endif
  if (byte >= 10)
  {
    return byte - 10 + 'a';
  }
  else
  {
    return byte + '0';
  }
}

static std::string HexEncode(const std::string &bytes)
{
  std::string ret;
  ret.reserve(bytes.size() * 2);
  for (std::string::size_type i = 0; i < bytes.size(); ++i)
  {
    unsigned char byte = static_cast<unsigned char>(bytes[i]);
    ret.push_back(HexEncode(byte >> 4));
    ret.push_back(HexEncode(byte & 0x0f));
  }
  return ret;
}

static std::string BytesMapping(const std::string &bytes,
                                const google::protobuf::FieldDescriptor *field_descriptor)
{
  if (field_descriptor->lowercase_name() == "trace_id" ||
      field_descriptor->lowercase_name() == "span_id" ||
      field_descriptor->lowercase_name() == "parent_span_id")
  {
    return HexEncode(bytes);
  }
  else
  {
    return opentelemetry::sdk::common::Base64Escape(bytes);
  }
}

static void ConvertGenericFieldToJson(nlohmann::json &value,
                                      const google::protobuf::Message &message,
                                      const google::protobuf::FieldDescriptor *field_descriptor);

static void ConvertListFieldToJson(nlohmann::json &value,
                                   const google::protobuf::Message &message,
                                   const google::protobuf::FieldDescriptor *field_descriptor);

// NOLINTBEGIN(misc-no-recursion)
static void ConvertGenericMessageToJson(nlohmann::json &value,
                                        const google::protobuf::Message &message)
{
  std::vector<const google::protobuf::FieldDescriptor *> fields_with_data;
  message.GetReflection()->ListFields(message, &fields_with_data);
  for (std::size_t i = 0; i < fields_with_data.size(); ++i)
  {
    const google::protobuf::FieldDescriptor *field_descriptor = fields_with_data[i];
    nlohmann::json &child_value = value[field_descriptor->camelcase_name()];
    if (field_descriptor->is_repeated())
    {
      ConvertListFieldToJson(child_value, message, field_descriptor);
    }
    else
    {
      ConvertGenericFieldToJson(child_value, message, field_descriptor);
    }
  }
}

void ConvertGenericFieldToJson(nlohmann::json &value,
                               const google::protobuf::Message &message,
                               const google::protobuf::FieldDescriptor *field_descriptor)
{
  switch (field_descriptor->cpp_type())
  {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
      value = message.GetReflection()->GetInt32(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
      // According to Protobuf specs 64-bit integer numbers in JSON-encoded payloads are encoded as
      // decimal strings, and either numbers or strings are accepted when decoding.
      value = std::to_string(message.GetReflection()->GetInt64(message, field_descriptor));
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
      value = message.GetReflection()->GetUInt32(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
      // According to Protobuf specs 64-bit integer numbers in JSON-encoded payloads are encoded as
      // decimal strings, and either numbers or strings are accepted when decoding.
      value = std::to_string(message.GetReflection()->GetUInt64(message, field_descriptor));
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
      std::string empty;
      if (field_descriptor->type() == google::protobuf::FieldDescriptor::TYPE_BYTES)
      {
        value = BytesMapping(
            message.GetReflection()->GetStringReference(message, field_descriptor, &empty),
            field_descriptor);
      }
      else
      {
        value = message.GetReflection()->GetStringReference(message, field_descriptor, &empty);
      }
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
      ConvertGenericMessageToJson(
          value, message.GetReflection()->GetMessage(message, field_descriptor, nullptr));
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
      value = message.GetReflection()->GetDouble(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
      value = message.GetReflection()->GetFloat(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
      value = message.GetReflection()->GetBool(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
      value = message.GetReflection()->GetEnumValue(message, field_descriptor);
      break;
    }
    default: {
      break;
    }
  }
}

void ConvertListFieldToJson(nlohmann::json &value,
                            const google::protobuf::Message &message,
                            const google::protobuf::FieldDescriptor *field_descriptor)
{
  auto field_size = message.GetReflection()->FieldSize(message, field_descriptor);

  switch (field_descriptor->cpp_type())
  {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedInt32(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
      for (int i = 0; i < field_size; ++i)
      {
        // According to Protobuf specs 64-bit integer numbers in JSON-encoded payloads are encoded
        // as decimal strings, and either numbers or strings are accepted when decoding.
        value.push_back(std::to_string(
            message.GetReflection()->GetRepeatedInt64(message, field_descriptor, i)));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedUInt32(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
      for (int i = 0; i < field_size; ++i)
      {
        // According to Protobuf specs 64-bit integer numbers in JSON-encoded payloads are encoded
        // as decimal strings, and either numbers or strings are accepted when decoding.
        value.push_back(std::to_string(
            message.GetReflection()->GetRepeatedUInt64(message, field_descriptor, i)));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
      std::string empty;
      if (field_descriptor->type() == google::protobuf::FieldDescriptor::TYPE_BYTES)
      {
        for (int i = 0; i < field_size; ++i)
        {
          value.push_back(BytesMapping(message.GetReflection()->GetRepeatedStringReference(
                                           message, field_descriptor, i, &empty),
                                       field_descriptor));
        }
      }
      else
      {
        for (int i = 0; i < field_size; ++i)
        {
          value.push_back(message.GetReflection()->GetRepeatedStringReference(
              message, field_descriptor, i, &empty));
        }
      }
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
      for (int i = 0; i < field_size; ++i)
      {
        nlohmann::json sub_value;
        ConvertGenericMessageToJson(
            sub_value, message.GetReflection()->GetRepeatedMessage(message, field_descriptor, i));
        value.push_back(std::move(sub_value));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedDouble(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedFloat(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedBool(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(
            message.GetReflection()->GetRepeatedEnumValue(message, field_descriptor, i));
      }
      break;
    }
    default: {
      break;
    }
  }
}

// NOLINTEND(misc-no-recursion) suppressing for performance as if implemented with stack needs
// Dynamic memory allocation
}  // namespace

class OPENTELEMETRY_LOCAL_SYMBOL OtlpFileSystemBackend : public OtlpFileAppender
{
public:
  explicit OtlpFileSystemBackend(const OtlpFileClientFileSystemOptions &options)
      : options_(options), is_initialized_{false}
  {
    file_ = std::make_shared<FileStats>();
    file_->is_shutdown.store(false);
    file_->rotate_index            = 0;
    file_->written_size            = 0;
    file_->left_flush_record_count = 0;
    file_->last_checkpoint         = 0;
    file_->record_count.store(0);
    file_->flushed_record_count.store(0);
  }

  ~OtlpFileSystemBackend() override
  {
    if (file_)
    {
      file_->background_thread_waker_cv.notify_all();
      std::unique_ptr<std::thread> background_flush_thread;
      {
        std::lock_guard<std::mutex> lock_guard{file_->background_thread_lock};
        file_->background_flush_thread.swap(background_flush_thread);
      }
      if (background_flush_thread && background_flush_thread->joinable())
      {
        background_flush_thread->join();
      }
    }
  }

  // Written size is not required to be precise, we can just ignore tsan report here.
  OPENTELEMETRY_SANITIZER_NO_THREAD void MaybeRotateLog(std::size_t data_size)
  {
    if (file_->written_size > 0 && file_->written_size + data_size > options_.file_size)
    {
      RotateLog();
    }
    CheckUpdate();
  }

  void Export(nostd::string_view data, std::size_t record_count) override
  {
    if (!is_initialized_.load(std::memory_order_acquire))
    {
      Initialize();
    }

    MaybeRotateLog(data.size());

    std::shared_ptr<FILE> out = OpenLogFile(true);
    if (!out)
    {
      return;
    }

    fwrite(data.data(), 1, data.size(), out.get());

    {
      std::lock_guard<std::mutex> lock_guard{file_->file_lock};

      file_->record_count += record_count;

      // Pipe file size always returns 0, we ignore the size limit of it.
      auto written_size = ftell(out.get());
      if (written_size >= 0)
      {
        file_->written_size = static_cast<std::size_t>(written_size);
      }

      if (options_.flush_count > 0)
      {
        if (file_->left_flush_record_count <= record_count)
        {
          file_->left_flush_record_count = options_.flush_count;

          fflush(out.get());

          file_->flushed_record_count.store(file_->record_count.load(std::memory_order_acquire),
                                            std::memory_order_release);
        }
        else
        {
          file_->left_flush_record_count -= record_count;
        }
      }
    }

    // Maybe need spawn a background thread to flush FILE
    SpawnBackgroundWorkThread();
  }

  bool ForceFlush(std::chrono::microseconds timeout) noexcept override
  {
    std::chrono::microseconds wait_interval = timeout / 256;
    if (wait_interval <= std::chrono::microseconds{0})
    {
      wait_interval = timeout;
    }
    // If set timeout to a large value, we limit the check interval to 256ms.
    // So we will not wait too long to shutdown the client when missing the finish notification.
    if (wait_interval > std::chrono::microseconds{256000})
    {
      wait_interval = std::chrono::microseconds{256000};
    }

    std::size_t current_wait_for_flush_count = file_->record_count.load(std::memory_order_acquire);

    while (timeout >= std::chrono::microseconds::zero())
    {
      // No more data to flush
      {
        if (file_->flushed_record_count.load(std::memory_order_acquire) >=
            current_wait_for_flush_count)
        {
          break;
        }
      }

      std::chrono::system_clock::time_point begin_time = std::chrono::system_clock::now();
      // Notify background thread to flush immediately
      {
        std::lock_guard<std::mutex> lock_guard{file_->background_thread_lock};
        if (!file_->background_flush_thread)
        {
          break;
        }
        file_->background_thread_waker_cv.notify_all();
      }

      // Wait result
      {
        std::unique_lock<std::mutex> lk(file_->background_thread_waiter_lock);
        file_->background_thread_waiter_cv.wait_for(lk, wait_interval);
      }

      std::chrono::system_clock::time_point end_time = std::chrono::system_clock::now();
      if (end_time - begin_time > std::chrono::microseconds{1})
      {
        timeout -= std::chrono::duration_cast<std::chrono::microseconds>(end_time - begin_time);
      }
      else
      {
        timeout -= std::chrono::microseconds{1};
      }
    }

    return timeout >= std::chrono::microseconds::zero();
  }

  bool Shutdown(std::chrono::microseconds timeout) noexcept override
  {
    file_->is_shutdown.store(true, std::memory_order_release);

    bool result = ForceFlush(timeout);
    return result;
  }

private:
  void Initialize()
  {
    if (is_initialized_.load(std::memory_order_acquire))
    {
      return;
    }

    // Double check
    std::string file_pattern;
    {
      std::lock_guard<std::mutex> lock_guard{file_->file_lock};
      if (is_initialized_.load(std::memory_order_acquire))
      {
        return;
      }
      is_initialized_.store(true, std::memory_order_release);

      file_->rotate_index = 0;
      ResetLogFile();

      char file_path[FileSystemUtil::kMaxPathSize];
      for (std::size_t i = 0; options_.file_size > 0 && i < options_.rotate_size; ++i)
      {
        FormatPath(file_path, sizeof(file_path), options_.file_pattern, i);
        std::size_t existed_file_size = FileSystemUtil::GetFileSize(file_path);

        // File size is also zero when it's not existed.
        if (existed_file_size < options_.file_size)
        {
          file_->rotate_index = i;
          break;
        }
      }

      file_pattern = options_.file_pattern;
    }

    // Reset the interval to check
    static std::time_t check_interval[128] = {0};
    // Some timezone contains half a hour, we use 1800s for the max check interval.
    if (check_interval[static_cast<int>('S')] == 0)
    {
      check_interval[static_cast<int>('R')] = 60;
      check_interval[static_cast<int>('T')] = 1;
      check_interval[static_cast<int>('F')] = 1800;
      check_interval[static_cast<int>('S')] = 1;
      check_interval[static_cast<int>('M')] = 60;
      check_interval[static_cast<int>('I')] = 1800;
      check_interval[static_cast<int>('H')] = 1800;
      check_interval[static_cast<int>('w')] = 1800;
      check_interval[static_cast<int>('d')] = 1800;
      check_interval[static_cast<int>('j')] = 1800;
      check_interval[static_cast<int>('m')] = 1800;
      check_interval[static_cast<int>('y')] = 1800;
      check_interval[static_cast<int>('Y')] = 1800;
    }

    {
      check_file_path_interval_ = 0;
      for (std::size_t i = 0; i + 1 < file_pattern.size(); ++i)
      {
        if (file_pattern[i] == '%')
        {
          int checked = static_cast<int>(file_pattern[i + 1]);
          if (checked > 0 && checked < 128 && check_interval[checked] > 0)
          {
            if (0 == check_file_path_interval_ ||
                check_interval[checked] < check_file_path_interval_)
            {
              check_file_path_interval_ = check_interval[checked];
            }
          }
        }
      }
    }

    OpenLogFile(false);
  }

  std::shared_ptr<FILE> OpenLogFile(bool destroy_content)
  {
    std::lock_guard<std::mutex> lock_guard{file_->file_lock};

    if (file_->current_file)
    {
      return file_->current_file;
    }

    ResetLogFile();

    char file_path[FileSystemUtil::kMaxPathSize + 1];
    std::size_t file_path_size = FormatPath(file_path, FileSystemUtil::kMaxPathSize,
                                            options_.file_pattern, file_->rotate_index);
    if (file_path_size <= 0)
    {
      OTEL_INTERNAL_LOG_ERROR("[OTLP FILE Client] Generate file path from pattern "
                              << options_.file_pattern << " failed");
      return nullptr;
    }
    file_path[file_path_size] = 0;

    std::shared_ptr<FILE> of = std::make_shared<FILE>();

    std::string directory_name = FileSystemUtil::DirName(file_path);
    if (!directory_name.empty())
    {
      int error_code = 0;
      if (!FileSystemUtil::IsExist(directory_name.c_str()))
      {
        FileSystemUtil::MkDir(directory_name.c_str(), true, 0);
        error_code = errno;
      }

      if (!FileSystemUtil::IsExist(directory_name.c_str()))
      {
#if !defined(__CYGWIN__) && defined(_WIN32)
        char error_message[256] = {0};
        strerror_s(error_message, sizeof(error_message) - 1, error_code);
#else
        char error_message[256] = {0};
        strerror_r(error_code, error_message, sizeof(error_message) - 1);
#endif
        OTEL_INTERNAL_LOG_ERROR("[OTLP FILE Client] Create directory \""
                                << directory_name << "\" failed.errno: " << error_code
                                << ", message: " << error_message);
      }
    }

    if (destroy_content && FileSystemUtil::IsExist(file_path))
    {
      FILE *trunc_file = nullptr;
      OTLP_FILE_OPEN(trunc_file, file_path, "wb");
      if (nullptr == trunc_file)
      {
        OTEL_INTERNAL_LOG_ERROR("[OTLP FILE Client] Open "
                                << static_cast<const char *>(file_path)
                                << " failed with pattern: " << options_.file_pattern);
        return nullptr;
      }
      fclose(trunc_file);
    }

    std::FILE *new_file = nullptr;
    OTLP_FILE_OPEN(new_file, file_path, "ab");
    if (nullptr == new_file)
    {
      std::string hint;
      if (!directory_name.empty())
      {
        hint = std::string(".The directory \"") + directory_name +
               "\" may not exist or may not be writable.";
      }
      OTEL_INTERNAL_LOG_ERROR("[OTLP FILE Client] Open "
                              << static_cast<const char *>(file_path)
                              << " failed with pattern: " << options_.file_pattern << hint);
      return nullptr;
    }
    of = std::shared_ptr<std::FILE>(new_file, fclose);

    fseek(of.get(), 0, SEEK_END);
    file_->written_size = static_cast<size_t>(ftell(of.get()));

    file_->current_file    = of;
    file_->last_checkpoint = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    file_->file_path.assign(file_path, file_path_size);

    // Create hardlink for alias
#if !defined(FS_DISABLE_LINK)
    if (!options_.alias_pattern.empty())
    {
      char alias_file_path[FileSystemUtil::kMaxPathSize + 1];
      std::size_t file_path_len = FormatPath(alias_file_path, sizeof(alias_file_path) - 1,
                                             options_.alias_pattern, file_->rotate_index);
      if (file_path_len <= 0)
      {
        OTEL_INTERNAL_LOG_ERROR("[OTLP FILE Client] Generate alias file path from "
                                << options_.alias_pattern << " failed");
        return file_->current_file;
      }

      if (file_path_len < sizeof(alias_file_path))
      {
        alias_file_path[file_path_len] = 0;
      }

      if (0 == strcasecmp(file_path, alias_file_path))
      {
        return file_->current_file;
      }

      int res =
          FileSystemUtil::Link(file_->file_path.c_str(), alias_file_path,
                               static_cast<int32_t>(FileSystemUtil::LinkOption::kForceRewrite));
      if (res != 0)
      {
#  if !defined(__CYGWIN__) && defined(_WIN32)
        // We can use FormatMessage to get error message.But it may be unicode and may not be
        // printed correctly. See
        // https://learn.microsoft.com/en-us/windows/win32/debug/retrieving-the-last-error-code for
        // more details
        OTEL_INTERNAL_LOG_ERROR("[OTLP FILE Client] Link " << file_->file_path << " to "
                                                           << alias_file_path
                                                           << " failed, errno: " << res);
#  else
        OTEL_INTERNAL_LOG_ERROR("[OTLP FILE Client] Link "
                                << file_->file_path << " to " << alias_file_path
                                << " failed, errno: " << res << ", message: " << strerror(res));
#  endif
        return file_->current_file;
      }
    }
#endif

    return file_->current_file;
  }

  void RotateLog()
  {
    std::lock_guard<std::mutex> lock_guard{file_->file_lock};
    if (options_.rotate_size > 0)
    {
      file_->rotate_index = (file_->rotate_index + 1) % options_.rotate_size;
    }
    else
    {
      file_->rotate_index = 0;
    }
    ResetLogFile();
  }

  void CheckUpdate()
  {
    if (check_file_path_interval_ <= 0)
    {
      return;
    }

    std::time_t current_checkpoint =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (current_checkpoint / check_file_path_interval_ ==
        file_->last_checkpoint / check_file_path_interval_)
    {
      return;
    }
    // Refresh checkpoint
    file_->last_checkpoint = current_checkpoint;

    char file_path[FileSystemUtil::kMaxPathSize + 1];
    size_t file_path_len =
        FormatPath(file_path, sizeof(file_path) - 1, options_.file_pattern, file_->rotate_index);
    if (file_path_len <= 0)
    {
      return;
    }

    std::string new_file_path;
    std::string old_file_path;
    new_file_path.assign(file_path, file_path_len);

    {
      // Lock for a short time
      std::lock_guard<std::mutex> lock_guard{file_->file_lock};
      old_file_path = file_->file_path;

      if (new_file_path == old_file_path)
      {
        // Refresh checking time
        return;
      }
    }

    std::string new_dir = FileSystemUtil::DirName(new_file_path);
    std::string old_dir = FileSystemUtil::DirName(old_file_path);

    // Reset rotate index when directory changes
    if (new_dir != old_dir)
    {
      file_->rotate_index = 0;
    }

    ResetLogFile();
  }

  void ResetLogFile()
  {
    // ResetLogFile is called in lock, do not lock again

    file_->current_file.reset();
    file_->last_checkpoint = 0;
    file_->written_size    = 0;
  }

  void SpawnBackgroundWorkThread()
  {
    if (options_.flush_interval <= std::chrono::microseconds{0})
    {
      return;
    }

    if (!file_)
    {
      return;
    }

#if OPENTELEMETRY_HAVE_EXCEPTIONS
    try
    {
#endif

      std::lock_guard<std::mutex> lock_guard_caller{file_->background_thread_lock};
      if (file_->background_flush_thread)
      {
        return;
      }

      std::shared_ptr<FileStats> concurrency_file = file_;
      std::chrono::microseconds flush_interval    = options_.flush_interval;
      file_->background_flush_thread.reset(new std::thread([concurrency_file, flush_interval]() {
        std::chrono::system_clock::time_point last_free_job_timepoint =
            std::chrono::system_clock::now();
        std::size_t last_record_count = 0;

        while (true)
        {
          std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
          // Exit flush thread if there is not data to flush more than one minute.
          if (now - last_free_job_timepoint > std::chrono::minutes{1})
          {
            break;
          }

          if (concurrency_file->is_shutdown.load(std::memory_order_acquire))
          {
            break;
          }

          {
            std::unique_lock<std::mutex> lk(concurrency_file->background_thread_waker_lock);
            concurrency_file->background_thread_waker_cv.wait_for(lk, flush_interval);
          }

          {
            std::size_t current_record_count =
                concurrency_file->record_count.load(std::memory_order_acquire);
            std::lock_guard<std::mutex> lock_guard{concurrency_file->file_lock};
            if (current_record_count != last_record_count)
            {
              last_record_count       = current_record_count;
              last_free_job_timepoint = std::chrono::system_clock::now();
            }

            if (concurrency_file->current_file)
            {
              fflush(concurrency_file->current_file.get());
            }

            concurrency_file->flushed_record_count.store(current_record_count,
                                                         std::memory_order_release);
          }

          concurrency_file->background_thread_waiter_cv.notify_all();
        }

        // Detach running thread because it will exit soon
        std::unique_ptr<std::thread> background_flush_thread;
        {
          std::lock_guard<std::mutex> lock_guard_inner{concurrency_file->background_thread_lock};
          background_flush_thread.swap(concurrency_file->background_flush_thread);
        }
        if (background_flush_thread && background_flush_thread->joinable())
        {
          background_flush_thread->detach();
        }
      }));
#if OPENTELEMETRY_HAVE_EXCEPTIONS
    }
    catch (std::exception &e)
    {
      OTEL_INTERNAL_LOG_WARN("[OTLP FILE Client] Try to spawn background but got a exception: "
                             << e.what() << ".Data writing may experience some delays.");
    }
    catch (...)
    {
      OTEL_INTERNAL_LOG_WARN(
          "[OTLP FILE Client] Try to spawn background but got a unknown exception.Data writing may "
          "experience some delays.");
    }
#endif
  }

private:
  OtlpFileClientFileSystemOptions options_;

  struct FileStats
  {
    std::atomic<bool> is_shutdown;
    std::size_t rotate_index;
    std::size_t written_size;
    std::size_t left_flush_record_count;
    std::shared_ptr<FILE> current_file;
    std::mutex file_lock;
    std::time_t last_checkpoint;
    std::string file_path;
    std::atomic<std::size_t> record_count;
    std::atomic<std::size_t> flushed_record_count;

    std::unique_ptr<std::thread> background_flush_thread;
    std::mutex background_thread_lock;
    std::mutex background_thread_waker_lock;
    std::condition_variable background_thread_waker_cv;
    std::mutex background_thread_waiter_lock;
    std::condition_variable background_thread_waiter_cv;
  };
  std::shared_ptr<FileStats> file_;

  std::atomic<bool> is_initialized_;
  std::time_t check_file_path_interval_{0};
};

class OPENTELEMETRY_LOCAL_SYMBOL OtlpFileOstreamBackend : public OtlpFileAppender
{
public:
  explicit OtlpFileOstreamBackend(const std::reference_wrapper<std::ostream> &os) : os_(os) {}

  ~OtlpFileOstreamBackend() override {}

  void Export(nostd::string_view data, std::size_t /*record_count*/) override
  {
    os_.get().write(data.data(), data.size());
  }

  bool ForceFlush(std::chrono::microseconds /*timeout*/) noexcept override
  {
    os_.get().flush();

    return true;
  }

  bool Shutdown(std::chrono::microseconds timeout) noexcept override { return ForceFlush(timeout); }

private:
  std::reference_wrapper<std::ostream> os_;
};

OtlpFileClient::OtlpFileClient(OtlpFileClientOptions &&options)
    : is_shutdown_(false), options_(std::move(options))
{
  if (nostd::holds_alternative<OtlpFileClientFileSystemOptions>(options_.backend_options))
  {
    backend_ = opentelemetry::nostd::shared_ptr<OtlpFileAppender>(new OtlpFileSystemBackend(
        nostd::get<OtlpFileClientFileSystemOptions>(options_.backend_options)));
  }
  else if (nostd::holds_alternative<std::reference_wrapper<std::ostream>>(options_.backend_options))
  {
    backend_ = opentelemetry::nostd::shared_ptr<OtlpFileAppender>(new OtlpFileOstreamBackend(
        nostd::get<std::reference_wrapper<std::ostream>>(options_.backend_options)));
  }
  else if (nostd::holds_alternative<opentelemetry::nostd::shared_ptr<OtlpFileAppender>>(
               options_.backend_options))
  {
    backend_ =
        nostd::get<opentelemetry::nostd::shared_ptr<OtlpFileAppender>>(options_.backend_options);
  }
}

OtlpFileClient::~OtlpFileClient()
{
  if (!IsShutdown())
  {
    Shutdown();
  }
}

// ----------------------------- File Client methods ------------------------------
opentelemetry::sdk::common::ExportResult OtlpFileClient::Export(
    const google::protobuf::Message &message,
    std::size_t record_count) noexcept
{
  if (is_shutdown_)
  {
    return ::opentelemetry::sdk::common::ExportResult::kFailure;
  }

  nlohmann::json json_request;
  // Convert from proto into json object
  ConvertGenericMessageToJson(json_request, message);

  std::string post_body_json =
      json_request.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace);
  if (options_.console_debug)
  {
    OTEL_INTERNAL_LOG_DEBUG("[OTLP FILE Client] Write body(Json)" << post_body_json);
  }

  if (backend_)
  {
    post_body_json += '\n';
    backend_->Export(post_body_json, record_count);
    return ::opentelemetry::sdk::common::ExportResult::kSuccess;
  }

  return ::opentelemetry::sdk::common::ExportResult::kFailure;
}

bool OtlpFileClient::ForceFlush(std::chrono::microseconds timeout) noexcept
{
  if (backend_)
  {
    return backend_->ForceFlush(timeout);
  }

  return true;
}

bool OtlpFileClient::Shutdown(std::chrono::microseconds timeout) noexcept
{
  is_shutdown_ = true;

  if (backend_)
  {
    return backend_->Shutdown(timeout);
  }

  return true;
}

bool OtlpFileClient::IsShutdown() const noexcept
{
  return is_shutdown_;
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
