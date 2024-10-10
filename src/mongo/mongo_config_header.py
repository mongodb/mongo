# run various configure checks. This script is intended to be
# imported and run in the context of generate_config_header.py
import json
import os
import platform
import subprocess
import tempfile
import threading
from typing import Dict

logfile_path: str = ""
loglock = threading.Lock()


def log_check(message):
    global loglock, logfile_path
    with loglock:
        with open(logfile_path, "a") as f:
            f.write(message + "\n")


class CompilerSettings:
    compiler_path: str = ""
    compiler_args: str = ""
    env_vars: dict = {}


class HeaderDefinition:
    def __init__(self, key: str, value: str = None) -> None:
        self.key = key
        self.value = value


def macos_get_sdk_path():
    result = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True)
    return result.stdout.strip()


def compile_check(source_text: str) -> bool:
    temp = None
    if platform.system() == "Windows":
        temp = tempfile.NamedTemporaryFile(suffix=".cpp", delete=False)
        temp.write(source_text.encode())
        temp.close()
        command = [
            CompilerSettings.compiler_path,
            "/c",  # only compile and assemble, don't link since we don't want to have to pass in all of the libs of the dependencies
            temp.name,
            *CompilerSettings.compiler_args.split(" "),
        ]
        log_check(" ".join(command[:-1] + [source_text]))
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            env={**os.environ.copy(), **CompilerSettings.env_vars},
        )
    else:
        command = [
            CompilerSettings.compiler_path,
            "-c",  # only compile and assemble, don't link since we don't want to have to pass in all of the libs of the dependencies
            "-x",
            "c++",
            *CompilerSettings.compiler_args.split(" "),
            "-",
        ]
        log_check(" ".join(command + [source_text]))
        result = subprocess.run(
            command,
            input=source_text,
            capture_output=True,
            text=True,
            env={**os.environ.copy(), **CompilerSettings.env_vars},
        )
    if result.returncode != 0:
        log_check(f"stdout:\n{result.stdout}")
        log_check(f"stderr:\n{result.stderr}")
    log_check(f"Exit code:\n{result.returncode}")
    log_check("--------------------------------------------------\n\n")
    if temp:
        os.unlink(temp.name)
    return result.returncode == 0


def func_check(function_name: str, header_path: str) -> bool:
    source_text = """
#include <assert.h>
#include %(header_path)s

int main(void) {
  return 0 ? %(function_name)s != nullptr : 1;
}
""" % {"header_path": header_path, "function_name": function_name}
    return compile_check(source_text)


def glibc_rseq_present_flag() -> list[HeaderDefinition]:
    log_check("[MONGO_CONFIG_GLIBC_RSEQ] Checking for __rseq_size...")

    if compile_check("""
    #include <sys/rseq.h>
    #include <stdlib.h>
    #include <stdio.h>

    int main() {
        printf("%d", __rseq_size);
        return EXIT_SUCCESS;
    }"""):
        return [HeaderDefinition("MONGO_CONFIG_GLIBC_RSEQ")]
    else:
        return []


def memset_s_present_flag() -> list[HeaderDefinition]:
    log_check("[MONGO_CONFIG_HAVE_MEMSET_S] Checking for memset_s...")

    if compile_check("""
        #define __STDC_WANT_LIB_EXT1__ 1
        #include <cstring>
        int main(int argc, char* argv[]) {
            void* data = nullptr;
            return memset_s(data, 0, 0, 0);
        }
        """):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_MEMSET_S")]
    else:
        return []


def strnlen_present_flag() -> list[HeaderDefinition]:
    if platform.system() == "Windows":
        # Match SCons behavior
        return []

    log_check("[MONGO_CONFIG_HAVE_STRNLEN] Checking for strnlen...")

    if func_check("strnlen", "<string.h>"):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_STRNLEN")]
    else:
        return []


def explicit_bzero_present_flag() -> list[HeaderDefinition]:
    log_check("[MONGO_CONFIG_HAVE_EXPLICIT_BZERO] Checking for explicit_bzero...")

    # Glibc 2.25+, OpenBSD 5.5+ and FreeBSD 11.0+ offer explicit_bzero, a secure way to zero memory
    if func_check("explicit_bzero", "<string.h>"):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_EXPLICIT_BZERO")]
    else:
        return []


def pthread_setname_np_present_flag() -> list[HeaderDefinition]:
    log_check("[MONGO_CONFIG_HAVE_PTHREAD_SETNAME_NP] Checking for pthread_setname_np...")

    if compile_check("""
        #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
        #endif
        #include <pthread.h>

        int main() {
            pthread_setname_np(pthread_self(), "test");
            return 0;
        }
        """):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_PTHREAD_SETNAME_NP")]
    else:
        return []


def fips_mode_set_present_flag() -> list[HeaderDefinition]:
    log_check("[MONGO_CONFIG_HAVE_FIPS_MODE_SET] Checking for fips mode set...")

    if compile_check("""
        #include <openssl/crypto.h>
        #include <openssl/evp.h>
        int main(void) { (void)FIPS_mode_set; return 0; }
        """):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_FIPS_MODE_SET")]
    else:
        return []


def asn1_present_flag() -> list[HeaderDefinition]:
    log_check("[MONGO_CONFIG_HAVE_ASN1_ANY_DEFINITIONS] Checking for asn1...")

    if compile_check("""
        #include <openssl/asn1.h>
        int main(void) { (void)d2i_ASN1_SEQUENCE_ANY; return 0; }
        """):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_ASN1_ANY_DEFINITIONS")]
    else:
        return []


def ssl_set_ecdh_auto_present_flag() -> list[HeaderDefinition]:
    log_check(
        "[MONGO_CONFIG_HAVE_SSL_SET_ECDH_AUTO] Checking if SSL_[CTX_]_set_ecdh_auto is supported..."
    )

    if compile_check("""
        #include <openssl/ssl.h>

        int main() {
            SSL_CTX_set_ecdh_auto(0, 0);
            SSL_set_ecdh_auto(0, 0);
            return 0;
        }
        """):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_SSL_SET_ECDH_AUTO")]
    else:
        return []


def ssl_ec_key_new_present_flag() -> list[HeaderDefinition]:
    log_check(
        "[MONGO_CONFIG_HAVE_SSL_EC_KEY_NEW] Checking if EC_KEY_new_by_curve_name is supported..."
    )

    if compile_check("""
        #include <openssl/ssl.h>
        #include <openssl/ec.h>

        int main() {
            SSL_CTX_set_tmp_ecdh(0, 0);
            EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
            EC_KEY_free(0);
            return 0;
        }
        """):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_SSL_EC_KEY_NEW")]
    else:
        return []


def posix_monotonic_clock_present_flag() -> list[HeaderDefinition]:
    log_check(
        "[MONGO_CONFIG_HAVE_POSIX_MONOTONIC_CLOCK] Checking if the POSIX monotonic clock is supported..."
    )

    if compile_check("""
        #include <unistd.h>
        #if !(defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0)
        #error POSIX clock_gettime not supported
        #elif !(defined(_POSIX_MONOTONIC_CLOCK) && _POSIX_MONOTONIC_CLOCK >= 0)
        #error POSIX monotonic clock not supported
        #endif
        int main(void) { return 0; }
        """):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_POSIX_MONOTONIC_CLOCK")]
    else:
        return []


def execinfo_backtrace_present_flag() -> list[HeaderDefinition]:
    log_check(
        "[MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE] Checking if execinfo backtrace is supported..."
    )
    if compile_check("""
        #include <execinfo.h>
        int main(void) { return backtrace != nullptr && backtrace_symbols != nullptr && backtrace_symbols_fd != nullptr; }
        """):
        return [HeaderDefinition("MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE")]
    else:
        return []


def extended_alignment_flag() -> list[HeaderDefinition]:
    def check_extended_alignment(size: int) -> bool:
        log_check(
            f"[MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT] Checking for extended alignment {size} for concurrency types..."
        )

        return compile_check(
            """
            #include <atomic>
            #include <mutex>
            #include <cstddef>

            static_assert(alignof(std::max_align_t) < {0}, "whatever");

            alignas({0}) std::mutex aligned_mutex;
            alignas({0}) std::atomic<int> aligned_atomic;

            struct alignas({0}) aligned_struct_mutex {{
                std::mutex m;
            }};

            struct alignas({0}) aligned_struct_atomic {{
                std::atomic<int> m;
            }};

            struct holds_aligned_mutexes {{
                alignas({0}) std::mutex m1;
                alignas({0}) std::mutex m2;
            }} hm;

            struct holds_aligned_atomics {{
                alignas({0}) std::atomic<int> a1;
                alignas({0}) std::atomic<int> a2;
            }} ha;
            int main(void) {{ return 0; }}
            """.format(size)
        )

    # If we don't have a specialized search sequence for this
    # architecture, assume 64 byte cache lines, which is pretty
    # standard. If for some reason the compiler can't offer that, try
    # 32.
    default_alignment_search_sequence = [64, 32]

    # The following are the target architectures for which we have
    # some knowledge that they have larger cache line sizes. In
    # particular, POWER8 uses 128 byte lines and zSeries uses 256. We
    # start at the goal state, and work down until we find something
    # the compiler can actualy do for us.
    extended_alignment_search_sequence = {
        "ppc64le": [128, 64, 32],
        "s390x": [256, 128, 64, 32],
    }

    for size in extended_alignment_search_sequence.get(
        platform.machine().lower(), default_alignment_search_sequence
    ):
        if check_extended_alignment(size):
            return [HeaderDefinition("MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT", size)]
    return []


def altivec_vbpermq_output_flag() -> list[HeaderDefinition]:
    if platform.machine().lower() != "ppc64le":
        return []

    # This checks for an altivec optimization we use in full text search.
    # Different versions of gcc appear to put output bytes in different
    # parts of the output vector produced by vec_vbpermq.  This configure
    # check looks to see which format the compiler produces.
    #
    # NOTE: This breaks cross compiles, as it relies on checking runtime functionality for the
    # environment we're in.  A flag to choose the index, or the possibility that we don't have
    # multiple versions to support (after a compiler upgrade) could solve the problem if we
    # eventually need them.
    def check_altivec_vbpermq_output(index: int) -> bool:
        log_check(
            f"[MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX] Checking for vec_vbperm output in index {index}..."
        )

        return compile_check(
            """
                #include <altivec.h>
                #include <cstring>
                #include <cstdint>
                #include <cstdlib>

                int main() {{
                    using Native = __vector signed char;
                    const size_t size = sizeof(Native);
                    const Native bits = {{ 120, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 8, 0 }};

                    uint8_t inputBuf[size];
                    std::memset(inputBuf, 0xFF, sizeof(inputBuf));

                    for (size_t offset = 0; offset <= size; offset++) {{
                        Native vec = vec_vsx_ld(0, reinterpret_cast<const Native*>(inputBuf));

                        uint64_t mask = vec_extract(vec_vbpermq(vec, bits), {0});

                        size_t initialZeros = (mask == 0 ? size : __builtin_ctzll(mask));
                        if (initialZeros != offset) {{
			    return 1;
                        }}

                        if (offset < size) {{
                            inputBuf[offset] = 0;  // Add an initial 0 for the next loop.
                        }}
                    }}

		    return 0;
            }}
            """.format(index)
        )

    for index in [0, 1]:
        if check_altivec_vbpermq_output(index):
            return [HeaderDefinition("MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX", index)]
    return []


def usdt_provider_flags() -> list[HeaderDefinition]:
    if platform.system() == "Darwin":
        # Match SCons behavior
        return []

    log_check("[MONGO_CONFIG_USDT_PROVIDER] Checking if SDT usdt provider is available...")
    if compile_check("""
        #include <sys/sdt.h>
        int main(void) { return 0; }
        """):
        return [
            HeaderDefinition("MONGO_CONFIG_USDT_ENABLED"),
            HeaderDefinition("MONGO_CONFIG_USDT_PROVIDER", "SDT"),
        ]
    return []


def get_config_header_substs():
    config_header_substs = (
        (
            "@mongo_config_altivec_vec_vbpermq_output_index@",
            "MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX",
        ),
        ("@mongo_config_debug_build@", "MONGO_CONFIG_DEBUG_BUILD"),
        ("@mongo_config_use_tracing_profiler@", "MONGO_CONFIG_USE_TRACING_PROFILER"),
        ("@mongo_config_have_execinfo_backtrace@", "MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE"),
        ("@mongo_config_have_explicit_bzero@", "MONGO_CONFIG_HAVE_EXPLICIT_BZERO"),
        ("@mongo_config_have_fips_mode_set@", "MONGO_CONFIG_HAVE_FIPS_MODE_SET"),
        ("@mongo_config_have_header_unistd_h@", "MONGO_CONFIG_HAVE_HEADER_UNISTD_H"),
        ("@mongo_config_have_memset_s@", "MONGO_CONFIG_HAVE_MEMSET_S"),
        ("@mongo_config_have_posix_monotonic_clock@", "MONGO_CONFIG_HAVE_POSIX_MONOTONIC_CLOCK"),
        ("@mongo_config_have_pthread_setname_np@", "MONGO_CONFIG_HAVE_PTHREAD_SETNAME_NP"),
        ("@mongo_config_have_ssl_ec_key_new@", "MONGO_CONFIG_HAVE_SSL_EC_KEY_NEW"),
        ("@mongo_config_have_ssl_set_ecdh_auto@", "MONGO_CONFIG_HAVE_SSL_SET_ECDH_AUTO"),
        ("@mongo_config_have_strnlen@", "MONGO_CONFIG_HAVE_STRNLEN"),
        ("@mongo_config_max_extended_alignment@", "MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT"),
        ("@mongo_config_ocsp_stapling_enabled@", "MONGO_CONFIG_OCSP_STAPLING_ENABLED"),
        ("@mongo_config_optimized_build@", "MONGO_CONFIG_OPTIMIZED_BUILD"),
        ("@mongo_config_have_asn1_any_definitions@", "MONGO_CONFIG_HAVE_ASN1_ANY_DEFINITIONS"),
        ("@mongo_config_ssl_provider@", "MONGO_CONFIG_SSL_PROVIDER"),
        ("@mongo_config_ssl@", "MONGO_CONFIG_SSL"),
        ("@mongo_config_usdt_enabled@", "MONGO_CONFIG_USDT_ENABLED"),
        ("@mongo_config_usdt_provider@", "MONGO_CONFIG_USDT_PROVIDER"),
        ("@mongo_config_use_libunwind@", "MONGO_CONFIG_USE_LIBUNWIND"),
        ("@mongo_config_use_raw_latches@", "MONGO_CONFIG_USE_RAW_LATCHES"),
        ("@mongo_config_wiredtiger_enabled@", "MONGO_CONFIG_WIREDTIGER_ENABLED"),
        ("@mongo_config_grpc@", "MONGO_CONFIG_GRPC"),
        ("@mongo_config_glibc_rseq@", "MONGO_CONFIG_GLIBC_RSEQ"),
        ("@mongo_config_tcmalloc_google@", "MONGO_CONFIG_TCMALLOC_GOOGLE"),
        ("@mongo_config_tcmalloc_gperf@", "MONGO_CONFIG_TCMALLOC_GPERF"),
        ("@mongo_config_streams@", "MONGO_CONFIG_STREAMS"),
    )
    return config_header_substs


def generate_config_header(
    compiler_path, compiler_args, env_vars, logpath, additional_inputs=[], extra_definitions={}
) -> Dict[str, str]:
    global logfile_path
    CompilerSettings.compiler_path = compiler_path
    CompilerSettings.compiler_args = compiler_args
    CompilerSettings.env_vars = {
        **json.loads(env_vars),
        **({"SDKROOT": macos_get_sdk_path()} if platform.system() == "Darwin" else {}),
    }
    logfile_path = logpath

    extra_definitions_dict = json.loads(extra_definitions)

    definitions: list[HeaderDefinition] = []

    definitions += glibc_rseq_present_flag()
    definitions += memset_s_present_flag()
    definitions += strnlen_present_flag()
    definitions += explicit_bzero_present_flag()
    definitions += pthread_setname_np_present_flag()
    if (
        extra_definitions_dict.get("MONGO_CONFIG_SSL_PROVIDER")
        == "MONGO_CONFIG_SSL_PROVIDER_OPENSSL"
    ):
        definitions += fips_mode_set_present_flag()
        definitions += asn1_present_flag()
        definitions += ssl_set_ecdh_auto_present_flag()
        definitions += ssl_ec_key_new_present_flag()
    definitions += posix_monotonic_clock_present_flag()
    definitions += execinfo_backtrace_present_flag()
    definitions += extended_alignment_flag()
    definitions += altivec_vbpermq_output_flag()
    definitions += usdt_provider_flags()
    # New checks can be added here

    for key, value in extra_definitions_dict.items():
        definitions.append(HeaderDefinition(key, value))

    define_map = {definition.key: definition.value or "1" for definition in definitions}
    subst_map = {}
    for subst, define in get_config_header_substs():
        if define not in define_map:
            log_check(f"{define} was unchecked and is unused.")
            subst_map[subst] = f"// #undef {define}"
        else:
            subst_map[subst] = f"#define {define} {define_map[define]}"
    return subst_map
