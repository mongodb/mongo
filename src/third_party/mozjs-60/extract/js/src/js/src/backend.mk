# THIS FILE WAS AUTOMATICALLY GENERATED. DO NOT EDIT.

DEFINES += -DNDEBUG=1 -DTRIMMED=1 -DENABLE_WASM_GLOBAL -DWASM_HUGE_MEMORY -DENABLE_SHARED_ARRAY_BUFFER -DEXPORT_JS_API
DIRS := editline build
export:: js-confdefs.h
GARBAGE += js-confdefs.h
EXTRA_MDDEPEND_FILES += js-confdefs.h.pp
js-confdefs.h: /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/python/mozbuild/mozbuild/action/process_define_files.py $(srcdir)/js-confdefs.h.in
	$(REPORT_BUILD)
	$(call py_action,file_generate,/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/python/mozbuild/mozbuild/action/process_define_files.py process_define_file js-confdefs.h $(MDDEPDIR)/js-confdefs.h.pp $(srcdir)/js-confdefs.h.in)

export:: js-config.h
GARBAGE += js-config.h
EXTRA_MDDEPEND_FILES += js-config.h.pp
js-config.h: /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/python/mozbuild/mozbuild/action/process_define_files.py $(srcdir)/js-config.h.in
	$(REPORT_BUILD)
	$(call py_action,file_generate,/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/python/mozbuild/mozbuild/action/process_define_files.py process_define_file js-config.h $(MDDEPDIR)/js-config.h.pp $(srcdir)/js-config.h.in)

export:: frontend/ReservedWordsGenerated.h
GARBAGE += frontend/ReservedWordsGenerated.h
EXTRA_MDDEPEND_FILES += frontend/ReservedWordsGenerated.h.pp
frontend/ReservedWordsGenerated.h: /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/frontend/GenerateReservedWords.py $(srcdir)/frontend/ReservedWords.h
	$(REPORT_BUILD)
	$(call py_action,file_generate,/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/frontend/GenerateReservedWords.py main frontend/ReservedWordsGenerated.h $(MDDEPDIR)/frontend/ReservedWordsGenerated.h.pp $(srcdir)/frontend/ReservedWords.h)

export:: selfhosted.out.h
GARBAGE += selfhosted.out.h
selfhosted.js: selfhosted.out.h ;
GARBAGE += selfhosted.js
EXTRA_MDDEPEND_FILES += selfhosted.out.h.pp
selfhosted.out.h: /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/builtin/embedjs.py $(srcdir)/js.msg $(srcdir)/builtin/TypedObjectConstants.h $(srcdir)/builtin/SelfHostingDefines.h $(srcdir)/builtin/Utilities.js $(srcdir)/builtin/Array.js $(srcdir)/builtin/AsyncIteration.js $(srcdir)/builtin/Classes.js $(srcdir)/builtin/Date.js $(srcdir)/builtin/Error.js $(srcdir)/builtin/Function.js $(srcdir)/builtin/Generator.js $(srcdir)/builtin/intl/Collator.js $(srcdir)/builtin/intl/CommonFunctions.js $(srcdir)/builtin/intl/CurrencyDataGenerated.js $(srcdir)/builtin/intl/DateTimeFormat.js $(srcdir)/builtin/intl/IntlObject.js $(srcdir)/builtin/intl/LangTagMappingsGenerated.js $(srcdir)/builtin/intl/NumberFormat.js $(srcdir)/builtin/intl/PluralRules.js $(srcdir)/builtin/intl/RelativeTimeFormat.js $(srcdir)/builtin/Iterator.js $(srcdir)/builtin/Map.js $(srcdir)/builtin/Module.js $(srcdir)/builtin/Number.js $(srcdir)/builtin/Object.js $(srcdir)/builtin/Promise.js $(srcdir)/builtin/Reflect.js $(srcdir)/builtin/RegExp.js $(srcdir)/builtin/RegExpGlobalReplaceOpt.h.js $(srcdir)/builtin/RegExpLocalReplaceOpt.h.js $(srcdir)/builtin/String.js $(srcdir)/builtin/Set.js $(srcdir)/builtin/Sorting.js $(srcdir)/builtin/TypedArray.js $(srcdir)/builtin/TypedObject.js $(srcdir)/builtin/WeakMap.js $(srcdir)/builtin/WeakSet.js
	$(REPORT_BUILD)
	$(call py_action,file_generate,/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/builtin/embedjs.py generate_selfhosted selfhosted.out.h $(MDDEPDIR)/selfhosted.out.h.pp $(srcdir)/js.msg $(srcdir)/builtin/TypedObjectConstants.h $(srcdir)/builtin/SelfHostingDefines.h $(srcdir)/builtin/Utilities.js $(srcdir)/builtin/Array.js $(srcdir)/builtin/AsyncIteration.js $(srcdir)/builtin/Classes.js $(srcdir)/builtin/Date.js $(srcdir)/builtin/Error.js $(srcdir)/builtin/Function.js $(srcdir)/builtin/Generator.js $(srcdir)/builtin/intl/Collator.js $(srcdir)/builtin/intl/CommonFunctions.js $(srcdir)/builtin/intl/CurrencyDataGenerated.js $(srcdir)/builtin/intl/DateTimeFormat.js $(srcdir)/builtin/intl/IntlObject.js $(srcdir)/builtin/intl/LangTagMappingsGenerated.js $(srcdir)/builtin/intl/NumberFormat.js $(srcdir)/builtin/intl/PluralRules.js $(srcdir)/builtin/intl/RelativeTimeFormat.js $(srcdir)/builtin/Iterator.js $(srcdir)/builtin/Map.js $(srcdir)/builtin/Module.js $(srcdir)/builtin/Number.js $(srcdir)/builtin/Object.js $(srcdir)/builtin/Promise.js $(srcdir)/builtin/Reflect.js $(srcdir)/builtin/RegExp.js $(srcdir)/builtin/RegExpGlobalReplaceOpt.h.js $(srcdir)/builtin/RegExpLocalReplaceOpt.h.js $(srcdir)/builtin/String.js $(srcdir)/builtin/Set.js $(srcdir)/builtin/Sorting.js $(srcdir)/builtin/TypedArray.js $(srcdir)/builtin/TypedObject.js $(srcdir)/builtin/WeakMap.js $(srcdir)/builtin/WeakSet.js)

export:: gc/StatsPhasesGenerated.h
GARBAGE += gc/StatsPhasesGenerated.h
EXTRA_MDDEPEND_FILES += gc/StatsPhasesGenerated.h.pp
gc/StatsPhasesGenerated.h: /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/gc/GenerateStatsPhases.py
	$(REPORT_BUILD)
	$(call py_action,file_generate,/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/gc/GenerateStatsPhases.py generateHeader gc/StatsPhasesGenerated.h $(MDDEPDIR)/gc/StatsPhasesGenerated.h.pp)

export:: gc/StatsPhasesGenerated.cpp
GARBAGE += gc/StatsPhasesGenerated.cpp
EXTRA_MDDEPEND_FILES += gc/StatsPhasesGenerated.cpp.pp
gc/StatsPhasesGenerated.cpp: /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/gc/GenerateStatsPhases.py
	$(REPORT_BUILD)
	$(call py_action,file_generate,/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/gc/GenerateStatsPhases.py generateCpp gc/StatsPhasesGenerated.cpp $(MDDEPDIR)/gc/StatsPhasesGenerated.cpp.pp)

CPPSRCS += builtin/RegExp.cpp
CPPSRCS += frontend/Parser.cpp
CPPSRCS += gc/StoreBuffer.cpp
CPPSRCS += jit/x86-shared/Disassembler-x86-shared.cpp
CPPSRCS += jsarray.cpp
CPPSRCS += jsmath.cpp
CPPSRCS += jsutil.cpp
CPPSRCS += perf/pm_linux.cpp
CPPSRCS += util/DoubleToString.cpp
CPPSRCS += vm/Interpreter.cpp
CPPSRCS += vm/JSAtom.cpp

# We build files in 'unified' mode by including several files
# together into a single source file.  This cuts down on
# compilation times and debug information size.
UNIFIED_CPPSRCS := Unified_cpp_js_src0.cpp Unified_cpp_js_src1.cpp Unified_cpp_js_src10.cpp Unified_cpp_js_src11.cpp Unified_cpp_js_src12.cpp Unified_cpp_js_src13.cpp Unified_cpp_js_src14.cpp Unified_cpp_js_src15.cpp Unified_cpp_js_src16.cpp Unified_cpp_js_src17.cpp Unified_cpp_js_src18.cpp Unified_cpp_js_src19.cpp Unified_cpp_js_src2.cpp Unified_cpp_js_src20.cpp Unified_cpp_js_src21.cpp Unified_cpp_js_src22.cpp Unified_cpp_js_src23.cpp Unified_cpp_js_src24.cpp Unified_cpp_js_src25.cpp Unified_cpp_js_src26.cpp Unified_cpp_js_src27.cpp Unified_cpp_js_src28.cpp Unified_cpp_js_src29.cpp Unified_cpp_js_src3.cpp Unified_cpp_js_src30.cpp Unified_cpp_js_src31.cpp Unified_cpp_js_src32.cpp Unified_cpp_js_src33.cpp Unified_cpp_js_src34.cpp Unified_cpp_js_src35.cpp Unified_cpp_js_src36.cpp Unified_cpp_js_src37.cpp Unified_cpp_js_src38.cpp Unified_cpp_js_src39.cpp Unified_cpp_js_src4.cpp Unified_cpp_js_src40.cpp Unified_cpp_js_src41.cpp Unified_cpp_js_src42.cpp Unified_cpp_js_src43.cpp Unified_cpp_js_src44.cpp Unified_cpp_js_src45.cpp Unified_cpp_js_src5.cpp Unified_cpp_js_src6.cpp Unified_cpp_js_src7.cpp Unified_cpp_js_src8.cpp Unified_cpp_js_src9.cpp
CPPSRCS += $(UNIFIED_CPPSRCS)
DoubleToString.cpp_FLAGS += -Wno-implicit-fallthrough
dist_include_FILES += js-config.h
dist_include_DEST := $(DEPTH)/dist/include/
dist_include_TARGET := export
INSTALL_TARGETS += dist_include
COMPUTED_LDFLAGS += -lpthread -Wl,-z,noexecstack -Wl,-z,text -Wl,-z,relro -Wl,--build-id -Wl,-rpath-link,/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/bin -Wl,-rpath-link,/usr/local/lib
LIBRARY_NAME := js_src
FORCE_STATIC_LIB := 1
REAL_LIBRARY := libjs_src.a
DEFINES += -DMOZ_HAS_MOZGLUE
COMPUTED_CXXFLAGS += -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/system_wrappers -include /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/config/gcc_hidden.h -DNDEBUG=1 -DTRIMMED=1 -DENABLE_WASM_GLOBAL -DWASM_HUGE_MEMORY -DENABLE_SHARED_ARRAY_BUFFER -DEXPORT_JS_API -DMOZ_HAS_MOZGLUE -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/js/src -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/include -fPIC -DMOZILLA_CLIENT -include /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/js/src/js-confdefs.h -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wempty-body -Wignored-qualifiers -Woverloaded-virtual -Wpointer-arith -Wsign-compare -Wtype-limits -Wunreachable-code -Wwrite-strings -Wno-invalid-offsetof -Wc++1z-compat -Wduplicated-cond -Wimplicit-fallthrough -Wno-error=maybe-uninitialized -Wno-error=deprecated-declarations -Wno-error=array-bounds -Wno-error=free-nonheap-object -Wformat -Wformat-overflow=2 -Wno-noexcept-type -fno-sized-deallocation -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fno-rtti -ffunction-sections -fdata-sections -fno-exceptions -fno-math-errno -pthread -pipe -g -freorder-blocks -O3 -fomit-frame-pointer -Wno-shadow -Werror=format -fno-strict-aliasing
COMPUTED_CXX_LDFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wempty-body -Wignored-qualifiers -Woverloaded-virtual -Wpointer-arith -Wsign-compare -Wtype-limits -Wunreachable-code -Wwrite-strings -Wno-invalid-offsetof -Wc++1z-compat -Wduplicated-cond -Wimplicit-fallthrough -Wno-error=maybe-uninitialized -Wno-error=deprecated-declarations -Wno-error=array-bounds -Wno-error=free-nonheap-object -Wformat -Wformat-overflow=2 -Wno-noexcept-type -fno-sized-deallocation -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fno-rtti -ffunction-sections -fdata-sections -fno-exceptions -fno-math-errno -pthread -pipe -g -freorder-blocks -O3 -fomit-frame-pointer
COMPUTED_C_LDFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wempty-body -Wignored-qualifiers -Wpointer-arith -Wsign-compare -Wtype-limits -Wunreachable-code -Wduplicated-cond -Wno-error=maybe-uninitialized -Wno-error=deprecated-declarations -Wno-error=array-bounds -Wno-error=free-nonheap-object -Wformat -Wformat-overflow=2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections -fno-math-errno -pthread -pipe -g -freorder-blocks -O3 -fomit-frame-pointer
COMPUTED_CFLAGS += -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/system_wrappers -include /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/config/gcc_hidden.h -DNDEBUG=1 -DTRIMMED=1 -DENABLE_WASM_GLOBAL -DWASM_HUGE_MEMORY -DENABLE_SHARED_ARRAY_BUFFER -DEXPORT_JS_API -DMOZ_HAS_MOZGLUE -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/js/src -I/home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/dist/include -fPIC -include /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/js/src/js-confdefs.h -DMOZILLA_CLIENT -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wempty-body -Wignored-qualifiers -Wpointer-arith -Wsign-compare -Wtype-limits -Wunreachable-code -Wduplicated-cond -Wno-error=maybe-uninitialized -Wno-error=deprecated-declarations -Wno-error=array-bounds -Wno-error=free-nonheap-object -Wformat -Wformat-overflow=2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections -fno-math-errno -pthread -pipe -g -freorder-blocks -O3 -fomit-frame-pointer
