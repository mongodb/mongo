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
	w="$w -Wmissing-parameter-type"
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
	w="$w -Wunreachable-code"
	w="$w -Wunsafe-loop-optimizations"
	w="$w -Wunused"
	w="$w -Wwrite-strings"

	# Non-fatal informational warnings.
	w="$w -Wno-error=inline"
	w="$w -Wno-error=unsafe-loop-optimizations"

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

	# Older OS X releases need some special love; these flags should be
	# removed in the not-too-distant future.
	# Apple clang version 4.1
	#	(tags/Apple/clang-421.11.66) (based on LLVM 3.1svn)
	w="$w -Wno-pedantic"
	w="$w -Wno-unused-command-line-argument"

	# Ignore unrecognized options.
	w="$w -Wno-unknown-warning-option"

	wt_cv_strict_warnings="$w"
])
