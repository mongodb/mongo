#include "binary/object.hpp"

#include "platform/platform.hpp"
#include "utils/utils.hpp"
#include "binary/module_base.hpp"
#include "logging.hpp"

#include <string>
#include <system_error>
#include <vector>
#include <mutex>
#include <unordered_map>

#if IS_LINUX || IS_APPLE
 #include <unistd.h>
 #include <dlfcn.h>
 #if IS_LINUX
  #include <link.h> // needed for dladdr1's link_map info
 #endif
#elif IS_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    #if IS_LINUX || IS_APPLE
    #if defined(CPPTRACE_HAS_DL_FIND_OBJECT) || defined(CPPTRACE_HAS_DLADDR1)
    std::string resolve_l_name(const char* l_name) {
        if(l_name != nullptr && l_name[0] != 0) {
            return l_name;
        } else {
            // empty l_name, this means it's the currently running executable
            // TODO: Caching and proper handling
            char buffer[CPPTRACE_PATH_MAX + 1]{};
            auto res = readlink("/proc/self/exe", buffer, CPPTRACE_PATH_MAX);
            if(res == -1) {
                return ""; // TODO
            } else {
                return buffer;
            }
        }
    }
    #endif
    // dladdr queries are needed to get pre-ASLR addresses and targets to run symbol resolution on
    // _dl_find_object is preferred if at all possible as it is much faster (added in glibc 2.35)
    // dladdr1 is preferred if possible because it allows for a more accurate object path to be resolved (glibc 2.3.3)
    #ifdef CPPTRACE_HAS_DL_FIND_OBJECT // we don't even check for this on apple
    object_frame get_frame_object_info(frame_ptr address) {
        // Use _dl_find_object when we can, it's orders of magnitude faster
        object_frame frame;
        frame.raw_address = address;
        frame.object_address = 0;
        dl_find_object result;
        if(_dl_find_object(reinterpret_cast<void*>(address), &result) == 0) { // thread safe
            frame.object_path = resolve_l_name(result.dlfo_link_map->l_name);
            frame.object_address = address - to_frame_ptr(result.dlfo_link_map->l_addr);
        }
        return frame;
    }
    #elif defined(CPPTRACE_HAS_DLADDR1)
    object_frame get_frame_object_info(frame_ptr address) {
        // https://github.com/bminor/glibc/blob/91695ee4598b39d181ab8df579b888a8863c4cab/elf/dl-addr.c#L26
        Dl_info info;
        link_map* link_map_info;
        object_frame frame;
        frame.raw_address = address;
        frame.object_address = 0;
        if(
            // thread safe
            dladdr1(reinterpret_cast<void*>(address), &info, reinterpret_cast<void**>(&link_map_info), RTLD_DL_LINKMAP)
        ) {
            frame.object_path = resolve_l_name(link_map_info->l_name);
            auto base = get_module_image_base(frame.object_path);
            if(base.has_value()) {
                frame.object_address = address
                                        - reinterpret_cast<std::uintptr_t>(info.dli_fbase)
                                        + base.unwrap_value();
            } else {
                if(!should_absorb_trace_exceptions()) {
                    base.drop_error();
                }
            }
        }
        return frame;
    }
    #else
    // glibc dladdr may not return an accurate dli_fname as it uses argv[0] for addresses in the main executable
    // https://github.com/bminor/glibc/blob/caed1f5c0b2e31b5f4e0f21fea4b2c9ecd3b5b30/elf/dl-addr.c#L33-L36
    // macos doesn't have dladdr1 but its dli_fname behaves more sensibly, same with some other libc's like musl
    object_frame get_frame_object_info(frame_ptr address) {
        // reference: https://github.com/bminor/glibc/blob/master/debug/backtracesyms.c
        Dl_info info;
        object_frame frame;
        frame.raw_address = address;
        frame.object_address = 0;
        if(dladdr(reinterpret_cast<void*>(address), &info)) { // thread safe
            frame.object_path = info.dli_fname;
            auto base = get_module_image_base(info.dli_fname);
            if(base.has_value()) {
                frame.object_address = address
                                        - reinterpret_cast<std::uintptr_t>(info.dli_fbase)
                                        + base.unwrap_value();
            } else {
                if(!should_absorb_trace_exceptions()) {
                    base.drop_error();
                }
            }
        }
        return frame;
    }
    #endif
    #else
    std::string get_module_name(HMODULE handle) {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        static std::unordered_map<HMODULE, std::string> cache;
        auto it = cache.find(handle);
        if(it == cache.end()) {
            char path[MAX_PATH];
            if(GetModuleFileNameA(handle, path, sizeof(path))) {
                cache.insert(it, {handle, path});
                return path;
            } else {
                log::error(std::system_error(GetLastError(), std::system_category()).what());
                cache.insert(it, {handle, ""});
                return "";
            }
        } else {
            return it->second;
        }
    }

    object_frame get_frame_object_info(frame_ptr address) {
        object_frame frame;
        frame.raw_address = address;
        frame.object_address = 0;
        HMODULE handle;
        // Multithread safe as long as another thread doesn't come along and free the module
        if(GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<const char*>(address),
            &handle
        )) {
            frame.object_path = get_module_name(handle);
            auto base = get_module_image_base(frame.object_path);
            if(base.has_value()) {
                frame.object_address = address
                                        - reinterpret_cast<std::uintptr_t>(handle)
                                        + base.unwrap_value();
            } else {
                if(!should_absorb_trace_exceptions()) {
                    base.drop_error();
                }
            }
        } else {
            log::error(std::system_error(GetLastError(), std::system_category()).what());
        }
        return frame;
    }
    #endif

    std::vector<object_frame> get_frames_object_info(const std::vector<frame_ptr>& addresses) {
        std::vector<object_frame> frames;
        frames.reserve(addresses.size());
        for(const frame_ptr address : addresses) {
            frames.push_back(get_frame_object_info(address));
        }
        return frames;
    }

    object_frame resolve_safe_object_frame(const safe_object_frame& frame) {
        std::string object_path = frame.object_path;
        if(object_path.empty()) {
            return {
                frame.raw_address,
                0,
                ""
            };
        }
        return {
            frame.raw_address,
            frame.address_relative_to_object_start,
            std::move(object_path)
        };
    }
}
CPPTRACE_END_NAMESPACE
