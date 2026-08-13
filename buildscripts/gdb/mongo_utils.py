"""Shared utilities used by the MongoDB GDB scripts.

This module is imported (not sourced) by other scripts in this directory. It
must remain free of module-level side effects so it can be safely loaded as a
module while those scripts are sourced by .gdbinit.
"""

import re
import sys
from collections.abc import Sequence

import gdb

MAIN_GLOBAL_BLOCK = None


def lookup_type(gdb_type_str: str) -> gdb.Type:
    """
    Try to find the type object from string.

    GDB says it searches the global blocks, however this appear not to be the
    case or at least it doesn't search all global blocks, sometimes it required
    to get the global block based off the current frame.
    """
    global MAIN_GLOBAL_BLOCK

    exceptions = []
    try:
        return gdb.lookup_type(gdb_type_str)
    except Exception as exc:
        exceptions.append(exc)

    if MAIN_GLOBAL_BLOCK is None:
        MAIN_GLOBAL_BLOCK = gdb.lookup_symbol("main")[0].symtab.global_block()

    try:
        return gdb.lookup_type(gdb_type_str, MAIN_GLOBAL_BLOCK)
    except Exception as exc:
        exceptions.append(exc)

    raise gdb.error("Failed to get type, tried:\n%s" % "\n".join([str(exc) for exc in exceptions]))


def lookup_symbol(
    gdb_symbol_str: str,
    function_pointer_type: str | None = None,
    *,
    function_signatures: Sequence[str] = (),
) -> gdb.Symbol | gdb.Value | None:
    """Look up a symbol in the current context and then in each loaded objfile.

    ``gdb.lookup_symbol`` does not always search symbols from dynamically loaded
    shared libraries.  Looking up the symbol on each objfile provides a fallback
    for symbols which are visible to GDB's CLI but not to the global Python API.
    The CLI fallback also handles non-debugging symbols, including C++ symbols
    whose demangled name contains an ABI tag such as ``[abi:cxx11]``.

    If ``function_pointer_type`` is provided, the result is cast to that function
    pointer type and dereferenced. This makes stripped function symbols callable
    when GDB has no debug information from which to infer their return type.

    ``function_signatures`` can be provided for a function which is present only
    in the binary's minimal symbol table. In that case, ``info address`` can
    locate the symbol even when ``info functions`` cannot. Each signature should
    be specific enough to select the desired overload. Multiple spellings allow
    for GDB output that either preserves or resolves C++ typedefs.
    """

    def cast_function_symbol(symbol):
        if function_pointer_type is None:
            return symbol

        symbol_value = getattr(symbol, "value", None)
        if callable(symbol_value):
            symbol_value = symbol_value()
        else:
            symbol_value = symbol

        # A value resolved from an exact C++ expression already has the
        # function's ABI-correct type, including its return type. In
        # particular, do not force GDB to resolve ``std::string`` again: that
        # can fail when split DWARF contains skeleton units that this GDB
        # version cannot process.
        symbol_type = getattr(symbol_value, "type", None)
        if getattr(symbol_type, "code", None) == getattr(gdb, "TYPE_CODE_FUNC", None):
            return symbol_value

        pointer_type = gdb.parse_and_eval(f"({function_pointer_type})0").type
        return symbol_value.cast(pointer_type).dereference()

    def cast_function_address(address: int):
        if function_pointer_type is None:
            return gdb.Value(address)

        pointer_type = gdb.parse_and_eval(f"({function_pointer_type})0").type
        return gdb.Value(address).cast(pointer_type).dereference()

    try:
        symbol, _ = gdb.lookup_symbol(gdb_symbol_str)
    except gdb.error:
        symbol = None
    if symbol is not None:
        try:
            return cast_function_symbol(symbol)
        except gdb.error:
            # Looking up a symbol and evaluating its value exercise different
            # DWARF paths. A split-DWARF error while evaluating the symbol must
            # not prevent the address-based fallbacks below.
            pass

    for objfile in gdb.objfiles():
        try:
            symbol = objfile.lookup_global_symbol(gdb_symbol_str)
        except gdb.error:
            # Some GDB versions reject split-DWARF skeleton units while using
            # the Python symbol APIs. Keep going because the CLI symbol table
            # queries below can still resolve the function's address.
            continue
        if symbol is not None:
            try:
                return cast_function_symbol(symbol)
            except gdb.error:
                continue

    for function_signature in function_signatures:
        try:
            address_info = gdb.execute(
                "info address " + function_signature,
                to_string=True,
            )
        except gdb.error:
            continue

        # GDB uses "is at 0x..." for minimal symbols and "is a function at
        # address 0x..." when debug information describes the function.
        match = re.search(r"\bat(?: address)?\s+(0x[0-9a-fA-F]+)\b", address_info)
        if match is not None:
            try:
                return cast_function_address(int(match.group(1), 16))
            except gdb.error:
                if function_pointer_type is not None:
                    # The address identifies the overload, but constructing a
                    # synthetic pointer type may fail when GDB cannot resolve
                    # split-DWARF type information. Ask GDB to evaluate the
                    # exact signature instead; this gives us the function's
                    # native, ABI-correct type.
                    try:
                        function = gdb.parse_and_eval("'" + function_signature + "'")
                        return cast_function_symbol(function)
                    except gdb.error:
                        pass

    try:
        functions = gdb.execute(
            "info functions -q " + gdb_symbol_str,
            to_string=True,
        )
    except gdb.error:
        return None

    for line in functions.splitlines():
        match = re.match(r"\s*(0x[0-9a-fA-F]+)\s+(.+?)\s*$", line)
        if match is None:
            continue

        address = int(match.group(1), 16)
        candidate = match.group(2).removesuffix("@plt").removesuffix(";")
        normalized_candidate = re.sub(r"\[abi:[^\]]+\]", "", candidate)
        if not normalized_candidate.startswith(gdb_symbol_str + "("):
            continue

        if function_pointer_type is not None:
            # Prefer the exact spelling from GDB. This preserves ABI tags such
            # as ``[abi:cxx11]`` and lets GDB return a typed function value
            # without requiring a second lookup of its return type. Keep the
            # normalized spelling for GDB versions that do not accept ABI tags
            # in expressions.
            for expression in (candidate, normalized_candidate):
                try:
                    function = gdb.parse_and_eval("'" + expression + "'")
                    return cast_function_symbol(function)
                except gdb.error:
                    continue

        try:
            # Use the address printed by GDB rather than asking it to resolve the
            # demangled overload again. The latter can fail for ABI-tagged C++
            # names even though ``info functions`` found the symbol.
            return cast_function_address(address)
        except gdb.error:
            continue

    return None


def get_thread_id():
    """Return the thread_id of the current GDB thread."""
    # GDB thread example:
    #  RHEL
    #   [Current thread is 1 (Thread 0x7f072426cca0 (LWP 12867))]
    thread_info = gdb.execute("thread", from_tty=False, to_string=True)

    if sys.platform.startswith("linux"):
        match = re.search(r"Thread (?P<pthread_id>0x[0-9a-f]+)", thread_info)
        if match:
            return int(match.group("pthread_id"), 16)
    elif sys.platform.startswith("sunos"):
        match = re.search(r"Thread (?P<pthread_id>[0-9]+)", thread_info)
        if match:
            return int(match.group("pthread_id"), 10)
        lwpid = gdb.selected_thread().ptid[1]
        if lwpid != 0:
            return lwpid
    raise ValueError("Failed to find thread id in {}".format(thread_info))


def get_current_thread_name():
    """Return the name of the current GDB thread."""
    fallback_name = '"%s"' % (gdb.selected_thread().name or "")
    try:
        # This goes through the pretty printer for std::string_view which adds "" around the name.
        name = str(gdb.parse_and_eval("mongo::getThreadName()"))
        if name == '""':
            return fallback_name
        return name
    except gdb.error:
        return fallback_name


class RegisterMongoCommand(object):
    """Class to register mongo commands with GDB."""

    _MONGO_COMMANDS = {}  # type: ignore

    @classmethod
    def register(cls, obj, name, command_class):
        """Register a command with no completer as a mongo command."""
        gdb.Command.__init__(obj, name, command_class)
        cls._MONGO_COMMANDS[name] = obj.__doc__

    @classmethod
    def print_commands(cls):
        """Print the registered mongo commands."""
        print("Command - Description")
        for key in cls._MONGO_COMMANDS:
            print("%s - %s" % (key, cls._MONGO_COMMANDS[key]))


def get_bytes(obj):
    """
    Returns a gdb.Value where its type resolves to `unsigned char*`. The caller must take care to
    cast the returned value themselves. This function is particularly useful in the context of
    mongo::Decorable<> types which store the decorations as a slab of memory with unsigned char*.
    """
    return obj.cast(gdb.lookup_type("unsigned char").pointer())


def get_unique_ptr_bytes(obj):
    """Read the value of a libstdc++ std::unique_ptr."""
    return obj.cast(gdb.lookup_type("std::_Head_base<0, unsigned char*, false>"))["_M_head_impl"]


def get_unique_ptr(obj):
    """Read the value of a libstdc++ std::unique_ptr."""
    return get_unique_ptr_bytes(obj).cast(obj.type.template_argument(0).pointer())


def _cast_decoration_value(type_name: str, decoration_address: int, /) -> gdb.Value:
    # We cannot use gdb.lookup_type() when the decoration type is a pointer type, e.g.
    # ServiceContext::declareDecoration<VectorClock*>(). gdb.parse_and_eval() is one of the few
    # ways to convert a type expression into a gdb.Type value. Some care is taken to quote the
    # non-pointer portion of the type so resolution for a type defined within an anonymous
    # namespace works correctly.
    type_name_regex = re.compile(r"^(.*[\w>])([\s\*]*)$")
    escaped = type_name_regex.sub(r"'\1'\2*", type_name)
    return gdb.parse_and_eval(f"({escaped}) {decoration_address}").dereference()


def get_object_decoration(decorable, start, index):
    decoration_data = get_bytes(decorable["_decorations"]["_data"])
    entry = start[index]
    deco_type_info = str(entry["typeInfo"])
    deco_type_name = re.sub(r".* <typeinfo for (.*)>", r"\1", deco_type_info)
    offset = int(entry["offset"])
    obj = decoration_data[offset]
    obj_addr = re.sub(r"^(.*) .*", r"\1", str(obj.address))
    obj = _cast_decoration_value(deco_type_name, int(obj.address))
    return (deco_type_name, obj, obj_addr)


def get_decorable_info(decorable):
    decorable_t = decorable.type.template_argument(0).name
    reg_sym, _ = gdb.lookup_symbol("mongo::decorable_detail::gdbRegistry<{}>".format(decorable_t))
    decl_vector = reg_sym.value()["_entries"]
    start = decl_vector["_M_impl"]["_M_start"]
    finish = decl_vector["_M_impl"]["_M_finish"]
    decinfo_t = lookup_type("mongo::decorable_detail::Registry::Entry")
    count = int((int(finish) - int(start)) / decinfo_t.sizeof)
    return start, count


def get_boost_optional(optional):
    """
    Retrieve the value stored in a boost::optional type, if it is non-empty.

    Returns None if the optional is empty.

    TODO: Import the boost pretty printers instead of using this custom function.
    """
    if not optional["m_initialized"]:
        return None
    value_ref_type = optional.type.template_argument(0).pointer()

    # boost::optional<T> is either stored using boost::optional_detail::aligned_storage<T> or
    # using direct storage of `T`. Scalar types are able to take advantage of direct storage.
    #
    # https://www.boost.org/doc/libs/1_79_0/libs/optional/doc/html/boost_optional/tutorial/performance_considerations.html
    if optional["m_storage"].type.strip_typedefs().pointer() == value_ref_type:
        return optional["m_storage"]

    storage = optional["m_storage"]["dummy_"]["data"]
    return storage.cast(value_ref_type).dereference()


# Cache for resolved gdb.Type values as they can be expensive to look up.
_type_cache: dict[str, gdb.Type] = {}


def _absl_namespace_prefix(type_name: str) -> str:
    """Return the concrete absl namespace prefix from a public absl type name.
    Abseil uses an inline namespace for versioning, so it may contain '::lts_20250512' in the middle of the symbol name.
    Examples:
      absl::flat_hash_map<...> -> absl
      absl::lts_20250512::flat_hash_map<...> -> absl::lts_20250512
    """
    match = re.match(r"^(absl(?:::[A-Za-z0-9_]+)?)::", type_name)
    if not match:
        raise RuntimeError(f"Could not determine absl namespace from type: {type_name}")
    return match.group(1)


def _lookup_absl_common_fields_storage_type(val):
    """Resolve the concrete CommonFields Storage type for an absl hash container."""
    container_type_name = str(val.type.strip_typedefs().name)
    ns = _absl_namespace_prefix(container_type_name)
    cache_key = f"{ns}::container_internal::internal_compressed_tuple::Storage<CommonFields>"
    if cache_key in _type_cache:
        return _type_cache[cache_key]

    # Clang and GCC may mangle the templates differently for the 0 size_t parameter, so we try both '0' and '0ul'.
    candidates = (
        f"{ns}::container_internal::internal_compressed_tuple::Storage<"
        f"{ns}::container_internal::CommonFields, 0ul, false>",
        f"{ns}::container_internal::internal_compressed_tuple::Storage<"
        f"{ns}::container_internal::CommonFields, 0, false>",
    )

    exceptions = []
    for type_str in candidates:
        try:
            resolved = lookup_type(type_str)
            _type_cache[cache_key] = resolved
            return resolved
        except Exception as exc:
            exceptions.append(exc)

    raise gdb.error(
        "Failed to resolve absl CommonFields storage type, tried:\n%s"
        % "\n".join([str(exc) for exc in exceptions])
    )


def absl_get_settings(val):
    """Gets the settings_ field for abseil (flat/node)_hash_(map/set)."""

    common_fields_storage_type = _lookup_absl_common_fields_storage_type(val)

    # The Hash, Eq, or Alloc functors may not be zero-sized objects.
    # mongo::LogicalSessionIdHash is one such example. An explicit cast is needed to
    # disambiguate which `value` member variable of the CompressedTuple is to be accessed.
    return val["settings_"].cast(common_fields_storage_type)["value"]


def absl_container_size(settings):
    return settings["size_"]["data_"] >> 17


def absl_get_nodes(val):
    """Return a generator of every node in absl::container_internal::raw_hash_set and derived classes."""
    settings = absl_get_settings(val)

    size = absl_container_size(settings)
    if size == 0:
        return

    capacity = int(settings["capacity_"])
    heap = settings["heap_or_soo_"]["heap"]
    ctrl = heap["control"]

    # Derive the underlying type stored in the container.
    slot_type = lookup_type(str(val.type.strip_typedefs().name) + "::slot_type").strip_typedefs()

    # Using the array of ctrl bytes, search for in-use slots and return them
    # https://github.com/abseil/abseil-cpp/blob/8a3caf7dea955b513a6c1b572a2423c6b4213402/absl/container/internal/raw_hash_set.h#L2108-L2113
    for item in range(capacity):
        ctrl_t = int(ctrl[item])
        if ctrl_t >= 0:
            yield heap["slot_array"]["p"].cast(slot_type.pointer())[item]
