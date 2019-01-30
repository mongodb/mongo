# THIS FILE WAS AUTOMATICALLY GENERATED. DO NOT EDIT.

DEFINES += -DNDEBUG=1 -DTRIMMED=1
CPPSRCS += e_acos.cpp
CPPSRCS += e_acosh.cpp
CPPSRCS += e_asin.cpp
CPPSRCS += e_atan2.cpp
CPPSRCS += e_atanh.cpp
CPPSRCS += e_cosh.cpp
CPPSRCS += e_exp.cpp
CPPSRCS += e_hypot.cpp
CPPSRCS += e_log.cpp
CPPSRCS += e_log10.cpp
CPPSRCS += e_log2.cpp
CPPSRCS += e_pow.cpp
CPPSRCS += e_sinh.cpp
CPPSRCS += e_sqrt.cpp
CPPSRCS += k_exp.cpp
CPPSRCS += s_asinh.cpp
CPPSRCS += s_atan.cpp
CPPSRCS += s_cbrt.cpp
CPPSRCS += s_ceil.cpp
CPPSRCS += s_ceilf.cpp
CPPSRCS += s_copysign.cpp
CPPSRCS += s_expm1.cpp
CPPSRCS += s_fabs.cpp
CPPSRCS += s_floor.cpp
CPPSRCS += s_floorf.cpp
CPPSRCS += s_log1p.cpp
CPPSRCS += s_nearbyint.cpp
CPPSRCS += s_rint.cpp
CPPSRCS += s_rintf.cpp
CPPSRCS += s_scalbn.cpp
CPPSRCS += s_tanh.cpp
CPPSRCS += s_trunc.cpp
CPPSRCS += s_truncf.cpp
COMPUTED_LDFLAGS += -lpthread -Wl,-z,noexecstack -Wl,-z,text -Wl,-z,relro -Wl,--build-id -Wl,-rpath-link,/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/bin -Wl,-rpath-link,/usr/local/lib
LIBRARY_NAME := modules_fdlibm_src
FORCE_STATIC_LIB := 1
REAL_LIBRARY := libmodules_fdlibm_src.a
DEFINES += -DMOZ_HAS_MOZGLUE
COMPUTED_CXXFLAGS += -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/system_wrappers -include /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/config/gcc_hidden.h -DNDEBUG=1 -DTRIMMED=1 -DMOZ_HAS_MOZGLUE -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/modules/fdlibm/src -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/modules/fdlibm/src -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/include -fPIC -DMOZILLA_CLIENT -include /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/js/src/js-confdefs.h -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wempty-body -Wignored-qualifiers -Woverloaded-virtual -Wpointer-arith -Wsign-compare -Wtype-limits -Wunreachable-code -Wwrite-strings -Wno-invalid-offsetof -Wc++1z-compat -Wduplicated-cond -Wimplicit-fallthrough -Wno-error=maybe-uninitialized -Wno-error=deprecated-declarations -Wno-error=array-bounds -Wno-error=free-nonheap-object -Wformat -Wformat-overflow=2 -Wno-noexcept-type -fno-sized-deallocation -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fno-rtti -ffunction-sections -fdata-sections -fno-exceptions -fno-math-errno -pthread -pipe -g -freorder-blocks -O3 -fomit-frame-pointer -Wno-parentheses -Wno-sign-compare
COMPUTED_CXX_LDFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wempty-body -Wignored-qualifiers -Woverloaded-virtual -Wpointer-arith -Wsign-compare -Wtype-limits -Wunreachable-code -Wwrite-strings -Wno-invalid-offsetof -Wc++1z-compat -Wduplicated-cond -Wimplicit-fallthrough -Wno-error=maybe-uninitialized -Wno-error=deprecated-declarations -Wno-error=array-bounds -Wno-error=free-nonheap-object -Wformat -Wformat-overflow=2 -Wno-noexcept-type -fno-sized-deallocation -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fno-rtti -ffunction-sections -fdata-sections -fno-exceptions -fno-math-errno -pthread -pipe -g -freorder-blocks -O3 -fomit-frame-pointer
COMPUTED_C_LDFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wempty-body -Wignored-qualifiers -Wpointer-arith -Wsign-compare -Wtype-limits -Wunreachable-code -Wduplicated-cond -Wno-error=maybe-uninitialized -Wno-error=deprecated-declarations -Wno-error=array-bounds -Wno-error=free-nonheap-object -Wformat -Wformat-overflow=2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections -fno-math-errno -pthread -pipe -g -freorder-blocks -O3 -fomit-frame-pointer
COMPUTED_CFLAGS += -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/system_wrappers -include /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/config/gcc_hidden.h -DNDEBUG=1 -DTRIMMED=1 -DMOZ_HAS_MOZGLUE -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/modules/fdlibm/src -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/modules/fdlibm/src -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/include -fPIC -include /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/js/src/js-confdefs.h -DMOZILLA_CLIENT -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wempty-body -Wignored-qualifiers -Wpointer-arith -Wsign-compare -Wtype-limits -Wunreachable-code -Wduplicated-cond -Wno-error=maybe-uninitialized -Wno-error=deprecated-declarations -Wno-error=array-bounds -Wno-error=free-nonheap-object -Wformat -Wformat-overflow=2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections -fno-math-errno -pthread -pipe -g -freorder-blocks -O3 -fomit-frame-pointer
