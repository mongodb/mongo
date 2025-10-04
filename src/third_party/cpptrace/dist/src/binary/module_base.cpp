#include "binary/module_base.hpp"

#include "platform/platform.hpp"
#include "utils/utils.hpp"

#include <string>
#include <mutex>
#include <unordered_map>

#if IS_LINUX || IS_APPLE
 #include <unistd.h>
 #include <dlfcn.h>
 #if IS_APPLE
  #include "binary/mach-o.hpp"
 #else
  #include "binary/elf.hpp"
 #endif
#elif IS_WINDOWS
 #include "binary/pe.hpp"
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    #if IS_LINUX
    Result<std::uintptr_t, internal_error> get_module_image_base(const std::string& object_path) {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        static std::unordered_map<std::string, std::uintptr_t> cache;
        auto it = cache.find(object_path);
        if(it == cache.end()) {
            // arguably it'd be better to release the lock while computing this, but also arguably it's good to not
            // have two threads try to do the same computation
            auto elf_object = open_elf_cached(object_path);
            // TODO: Cache the error
            if(!elf_object) {
                return elf_object.unwrap_error();
            }
            auto base = elf_object.unwrap_value()->get_module_image_base();
            if(base.is_error()) {
                return std::move(base).unwrap_error();
            }
            cache.insert(it, {object_path, base.unwrap_value()});
            return base;
        } else {
            return it->second;
        }
    }
    #elif IS_APPLE
    Result<std::uintptr_t, internal_error> get_module_image_base(const std::string& object_path) {
        // We have to parse the Mach-O to find the offset of the text section.....
        // I don't know how addresses are handled if there is more than one __TEXT load command. I'm assuming for
        // now that there is only one, and I'm using only the first section entry within that load command.
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        static std::unordered_map<std::string, std::uintptr_t> cache;
        auto it = cache.find(object_path);
        if(it == cache.end()) {
            // arguably it'd be better to release the lock while computing this, but also arguably it's good to not
            // have two threads try to do the same computation
            auto mach_o_object = open_mach_o_cached(object_path);
            // TODO: Cache the error
            if(!mach_o_object) {
                return mach_o_object.unwrap_error();
            }
            auto base = mach_o_object.unwrap_value()->get_text_vmaddr();
            if(!base) {
                return std::move(base).unwrap_error();
            }
            cache.insert(it, {object_path, base.unwrap_value()});
            return base;
        } else {
            return it->second;
        }
    }
    #else // Windows
    Result<std::uintptr_t, internal_error> get_module_image_base(const std::string& object_path) {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        static std::unordered_map<std::string, std::uintptr_t> cache;
        auto it = cache.find(object_path);
        if(it == cache.end()) {
            // arguably it'd be better to release the lock while computing this, but also arguably it's good to not
            // have two threads try to do the same computation
            auto base = pe_get_module_image_base(object_path);
            // TODO: Cache the error
            if(!base) {
                return std::move(base).unwrap_error();
            }
            cache.insert(it, {object_path, base.unwrap_value()});
            return base;
        } else {
            return it->second;
        }
    }
    #endif
}
CPPTRACE_END_NAMESPACE
