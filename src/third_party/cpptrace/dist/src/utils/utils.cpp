#include "utils/utils.hpp"
#include "utils/string_view.hpp"

#if IS_WINDOWS
 #include <io.h>
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <sys/stat.h>
 #include <unistd.h>
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {

    bool isatty(int fd) {
        #if IS_WINDOWS
         return _isatty(fd);
        #else
         return ::isatty(fd);
        #endif
    }

    int fileno(std::FILE* stream) {
        #if IS_WINDOWS
         return _fileno(stream);
        #else
         return ::fileno(stream);
        #endif
    }

    void enable_virtual_terminal_processing_if_needed() noexcept {
        // enable colors / ansi processing if necessary
        #if IS_WINDOWS
         // https://docs.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#example-of-enabling-virtual-terminal-processing
         #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
          constexpr DWORD ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x4;
         #endif
         HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
         DWORD dwMode = 0;
         if(hOut == INVALID_HANDLE_VALUE) return;
         if(!GetConsoleMode(hOut, &dwMode)) return;
         if(dwMode != (dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
         if(!SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return;
        #endif
    }

    bool directory_exists(cstring_view path) {
        #if IS_WINDOWS
         DWORD dwAttrib = GetFileAttributesA(path.c_str());
         return dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY);
        #else
         struct stat sb;
         return stat(path.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
        #endif
    }

}
CPPTRACE_END_NAMESPACE
