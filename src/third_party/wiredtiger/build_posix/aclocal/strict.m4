# AM_STRICT
#	Per compiler-version flags used when compiling in strict mode.

# GCC warnings.
AC_DEFUN([AM_GCC_WARNINGS], [
	w="$w -Wall -Wextra -Werror"

	w="$w -Waggregate-return"
	w="$w -Wbad-function-cast"
	w="$w -Wcast-align"
	w="$w -Wdeclaration-after-statement"
	w="$w -Wdouble-promotion"
	w="$w -Wfloat-equal"
	w="$w -Wformat-nonliteral"
	w="$w -Wformat-security"
	w="$w -Wformat=2"
	w="$w -Winit-self"
	w="$w -Wjump-misses-init"
	w="$w -Wmissing-declarations"
	w="$w -Wmissing-field-initializers"
	w="$w -Wmissing-prototypes"
	w="$w -Wnested-externs"
	w="$w -Wold-style-definition"
	w="$w -Wpacked"
	w="$w -Wpointer-arith"
	w="$w -Wpointer-sign"
	w="$w -Wredundant-decls"
	w="$w -Wshadow"
	w="$w -Wsign-conversion"
	w="$w -Wstrict-prototypes"
	w="$w -Wswitch-enum"
	w="$w -Wundef"
	w="$w -Wuninitialized"
	w="$w -Wunreachable-code"
	w="$w -Wunused"
	w="$w -Wwrite-strings"

	# Non-fatal informational warnings.
	# We don't turn on the unsafe-loop-optimizations warning after gcc7,
	# it's too noisy to tolerate. Regardless, don't fail even when it's
	# configured.
	w="$w -Wno-error=unsafe-loop-optimizations"

	# GCC 4.7
	#	WiredTiger uses anonymous structures/unions, a C11 extension,
	#	turn off those warnings.
	gcc5=0
	gcc6=0
	gcc7=0
	gcc8=0
	case "$1" in
	[*4.7.[0-9]*])					# gcc4.7
		w="$w -Wno-c11-extensions"
		w="$w -Wunsafe-loop-optimizations";;
	[*5.[0-9].[0-9]*])				# gcc5.X
		w="$w -Wunsafe-loop-optimizations"
		gcc5=1;;
	[*6.[0-9].[0-9]*])				# gcc6.X
		w="$w -Wunsafe-loop-optimizations"
		gcc5=1
		gcc6=1;;
	[*7.[0-9].[0-9]*])				# gcc7.X
		gcc5=1
		gcc6=1
		gcc7=1;;
	[*8.[0-9].[0-9]*])				# gcc8.X
		gcc5=1
		gcc6=1
		gcc7=1
		gcc8=1;;
	esac

	if test $gcc5 -eq 1; then
		w="$w -Wformat-signedness"
		w="$w -Wjump-misses-init"
		w="$w -Wredundant-decls"
		w="$w -Wunused-macros"
		w="$w -Wvariadic-macros"
	fi
	if test $gcc6 -eq 1; then
		w="$w -Wduplicated-cond"
		w="$w -Wlogical-op"
		w="$w -Wunused-const-variable=2"
	fi
	if test $gcc7 -eq 1; then
		w="$w -Walloca"
		w="$w -Walloc-zero"
		w="$w -Wduplicated-branches"
		w="$w -Wformat-overflow=2"
		w="$w -Wformat-truncation=2"
		w="$w -Wrestrict"
	fi
	if test $gcc8 -eq 1; then
		w="$w -Wmultistatement-macros"
	fi

	wt_cv_strict_warnings="$w"
])

# Clang warnings.
AC_DEFUN([AM_CLANG_WARNINGS], [
	w="-Weverything -Werror"

	w="$w -Wno-cast-align"
	w="$w -Wno-documentation-unknown-command"
	w="$w -Wno-format-nonliteral"
	w="$w -Wno-packed"
	w="$w -Wno-padded"
	w="$w -Wno-reserved-id-macro"
	w="$w -Wno-zero-length-array"

	# We should turn on cast-qual, but not as a fatal error: see WT-2690.
	# For now, turn it off.
	# w="$w -Wno-error=cast-qual"
	w="$w -Wno-cast-qual"

	# Turn off clang thread-safety-analysis, it doesn't like some of the
	# code patterns in WiredTiger.
	w="$w -Wno-thread-safety-analysis"

	# On Centos 7.3.1611, system header files aren't compatible with
	# -Wdisabled-macro-expansion.
	w="$w -Wno-disabled-macro-expansion"

	case "$1" in
	*Apple*clang*version*4.1*)
		# Apple clang has its own numbering system, and older OS X
		# releases need some special love. Turn off some flags for
		# Apple's clang 4.1:
		#	Apple clang version 4.1
		#	(tags/Apple/clang-421.11.66) (based on LLVM 3.1svn)
		w="$w -Wno-attributes"
		w="$w -Wno-pedantic"
		w="$w -Wno-unused-command-line-argument";;
	esac

	# We occasionally use an extra semicolon to indicate an empty loop or
	# conditional body.
	w="$w -Wno-extra-semi-stmt"

	# FIXME-WT-8052: Figure out whether we want to disable these or change the code.
	w="$w -Wno-implicit-int-float-conversion"
	w="$w -Wno-implicit-fallthrough"
	w="$w -Wno-maybe-uninitialized"

	# Ignore unrecognized options.
	w="$w -Wno-unknown-warning-option"

	wt_cv_strict_warnings="$w"
])
