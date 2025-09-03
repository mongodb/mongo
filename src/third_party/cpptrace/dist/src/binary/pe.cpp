#include "binary/pe.hpp"

#include "platform/platform.hpp"
#include "utils/error.hpp"
#include "utils/utils.hpp"

#if IS_WINDOWS
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    template<typename T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    T pe_byteswap_if_needed(T value) {
        // PE header values are little endian, I think dos e_lfanew should be too
        if(!is_little_endian()) {
            return byteswap(value);
        } else {
            return value;
        }
    }

    Result<std::uintptr_t, internal_error> pe_get_module_image_base(cstring_view object_path) {
        // https://drive.google.com/file/d/0B3_wGJkuWLytbnIxY1J5WUs4MEk/view?pli=1&resourcekey=0-n5zZ2UW39xVTH8ZSu6C2aQ
        // https://0xrick.github.io/win-internals/pe3/
        // Endianness should always be little for dos and pe headers
        std::FILE* file_ptr;
        errno_t ret = fopen_s(&file_ptr, object_path.c_str(), "rb");
        auto file = raii_wrap(std::move(file_ptr), file_deleter);
        if(ret != 0 || file == nullptr) {
            return internal_error("Unable to read object file {}", object_path);
        }
        auto magic = load_bytes<std::array<char, 2>>(file, 0);
        if(!magic) {
            return std::move(magic).unwrap_error();
        }
        if(std::memcmp(magic.unwrap_value().data(), "MZ", 2) != 0) {
            return internal_error("File is not a PE file {}", object_path);
        }
        auto e_lfanew = load_bytes<DWORD>(file, 0x3c); // dos header + 0x3c
        if(!e_lfanew) {
            return std::move(e_lfanew).unwrap_error();
        }
        DWORD nt_header_offset = pe_byteswap_if_needed(e_lfanew.unwrap_value());
        auto signature = load_bytes<std::array<char, 4>>(file, nt_header_offset); // nt header + 0
        if(!signature) {
            return std::move(signature).unwrap_error();
        }
        if(std::memcmp(signature.unwrap_value().data(), "PE\0\0", 4) != 0) {
            return internal_error("File is not a PE file {}", object_path);
        }
        auto size_of_optional_header_raw = load_bytes<WORD>(file, nt_header_offset + 4 + 0x10); // file header + 0x10
        if(!size_of_optional_header_raw) {
            return std::move(size_of_optional_header_raw).unwrap_error();
        }
        WORD size_of_optional_header = pe_byteswap_if_needed(size_of_optional_header_raw.unwrap_value());
        if(size_of_optional_header == 0) {
            return internal_error("Unexpected optional header size for PE file");
        }
        auto optional_header_magic_raw = load_bytes<WORD>(file, nt_header_offset + 0x18); // optional header + 0x0
        if(!optional_header_magic_raw) {
            return std::move(optional_header_magic_raw).unwrap_error();
        }
        WORD optional_header_magic = pe_byteswap_if_needed(optional_header_magic_raw.unwrap_value());
        VERIFY(
            optional_header_magic == IMAGE_NT_OPTIONAL_HDR_MAGIC,
            ("PE file does not match expected bit-mode " + std::string(object_path)).c_str()
        );
        // finally get image base
        if(optional_header_magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            // 32 bit
            auto bytes = load_bytes<DWORD>(file, nt_header_offset + 0x18 + 0x1c); // optional header + 0x1c
            if(!bytes) {
                return std::move(bytes).unwrap_error();
            }
            return to<std::uintptr_t>(pe_byteswap_if_needed(bytes.unwrap_value()));
        } else {
            // 64 bit
            // I get an "error: 'QWORD' was not declared in this scope" for some reason when using QWORD
            auto bytes = load_bytes<std::uint64_t>(file, nt_header_offset + 0x18 + 0x18); // optional header + 0x18
            if(!bytes) {
                return std::move(bytes).unwrap_error();
            }
            return to<std::uintptr_t>(pe_byteswap_if_needed(bytes.unwrap_value()));
        }
    }
}
CPPTRACE_END_NAMESPACE

#endif
