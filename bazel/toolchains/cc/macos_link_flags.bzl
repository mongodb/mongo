"""Link flags shared by native and cross macOS toolchains."""

# LLD 19 can emit duplicate compact-unwind entries for empty code sections.
# The zero encoding shadows the following function's real entry and prevents
# exceptions from reaching catch handlers (LLVM issue #216154). Dead stripping
# removes those empty sections before LLD constructs the unwind table.
MACOS_LLD_LINK_FLAGS = ["-Wl,-dead_strip"]
