def _sanitize_label(s):
    # Mirror Python: allow [A-Za-z0-9_]; replace others with '_';
    # and if first char is a digit, prefix an underscore.
    out = []
    for i in range(len(s)):
        ch = s[i:i + 1]

        # isalnum() isn't available in Starlark; check ranges
        is_upper = "A" <= ch and ch <= "Z"
        is_lower = "a" <= ch and ch <= "z"
        is_digit = "0" <= ch and ch <= "9"
        if is_upper or is_lower or is_digit or ch == "_":
            if i == 0 and is_digit:
                out.append("_")
            out.append(ch)
        else:
            out.append("_")
    return "".join(out) if out else "_"

def dedupe_stable(xs):
    seen = {}
    out = []
    for x in xs:
        if x not in seen:
            seen[x] = True
            out.append(x)
    return out

def _fg_name_for_filename(name):
    # NEW: collapse to leaf to match Python generator
    leaf = name.rsplit("/", 1)[-1]
    if leaf.endswith("_gen"):
        return leaf
    i = leaf.rfind(".")
    if i == -1:
        return leaf
    base = leaf[:i]
    ext = leaf[i + 1:].replace(".", "_")
    return base + ("_" + ext if ext else "")

def _split_label_or_file(src):
    if type(src) != "string":
        fail("src must be string, got: {}".format(type(src)))
    if src.startswith("@"):
        dslash = src.find("//")
        if dslash < 0:
            fail("Invalid label (missing //): {}".format(src))
        rest = src[dslash + 2:]
        colon = rest.rfind(":")
        if colon < 0:
            fail("Invalid label (missing :): {}".format(src))
        return rest[:colon], rest[colon + 1:]
    if src.startswith("//"):
        no_slash = src[2:]
        colon = no_slash.rfind(":")
        if colon < 0:
            pkg = no_slash
            name = pkg.rsplit("/", 1)[-1]
            return pkg, name
        return no_slash[:colon], no_slash[colon + 1:]
    if src.startswith(":"):
        return native.package_name(), src[1:]
    return native.package_name(), src

def _auto_header_label(pkg, name):
    name = name.lstrip(":")
    return "//{}/.auto_header:{}".format(pkg, _sanitize_label(_fg_name_for_filename(name)))

def _is_third_party_pkg(pkg):
    # Bazel package path, no leading slashes, e.g. "src/third_party/node"
    return (
        pkg == "src/third_party" or
        pkg.startswith("src/third_party/") or
        pkg.startswith("third_party/") or
        "third_party/" in pkg  # safety
    )

def maybe_compute_auto_headers(srcs):
    # Only handle plain list-of-strings; if configurable/mixed, return None
    if type(srcs) != "list":
        return None

    out = []
    for s in srcs:
        if type(s) != "string":
            return None  # select()/mixed — let caller fall back

        # Already an auto-header label? keep as-is.
        if "/.auto_header:" in s:
            out.append(s)
            continue

        pkg, name = _split_label_or_file(s)
        if _is_third_party_pkg(pkg):
            continue

        # Skip external repos and any third_party package entirely
        if s.startswith("@"):
            continue

        # If *_gen listed in srcs, add its auto-header (transitive headers),
        # but do NOT add another ':name' here.
        if name.endswith("_gen"):
            out.append(_auto_header_label(pkg, name))  # //pkg/.auto_header:foo_gen
            continue

        # Regular mapping for files we care about
        if (name.endswith(".c") or name.endswith(".cc") or name.endswith(".cpp") or name.endswith(".cxx") or
            name.endswith(".h") or name.endswith(".hh") or name.endswith(".hpp") or name.endswith(".hxx")):
            out.append(_auto_header_label(pkg, name))
            continue

        # else: ignore

    return dedupe_stable(out)

def _all_headers_label_for_pkg(pkg):
    if pkg.startswith("src/mongo/db/modules/enterprise"):
        return ["//src/mongo/db/modules/enterprise/.auto_header:all_headers"]
    elif pkg.startswith("src/mongo/db/modules/atlas"):
        return ["//src/mongo/db/modules/atlas/.auto_header:all_headers"]
    else:
        return ["//bazel/auto_header/.auto_header:all_headers"]

def maybe_all_headers(name, hdrs, srcs, private_hdrs):
    pkg = native.package_name()
    if _is_third_party_pkg(pkg) or not pkg.startswith("src/mongo"):
        return hdrs, srcs + private_hdrs
    if not (pkg.startswith("src/mongo") or "third_party" in pkg):
        return hdrs, srcs + private_hdrs

    # 1) Wrap user-provided (possibly configurable) hdrs into a helper filegroup.
    #    This isolates any select(...) inside the filegroup's srcs where it's legal.
    hdr_wrap = name + "_hdrs_wrap"
    native.filegroup(
        name = hdr_wrap,
        srcs = hdrs,  # hdrs may already have select(...) — that's fine here
        visibility = ["//visibility:private"],
    )

    # 2) Always-on config header (added outside the select to avoid duplication)
    mongo_cfg_hdr = ["//src/mongo:mongo_config_header"]

    # 3) Select between the per-package all_headers filegroup and the wrapped hdrs.
    #    IMPORTANT: both branches are *plain label lists* -> no nested selects.
    final_hdrs = (
        mongo_cfg_hdr +
        select({
            "//bazel/config:all_headers_enabled": _all_headers_label_for_pkg(pkg),
            "//conditions:default": [":" + hdr_wrap],
        })
    )

    # 4) For srcs: include private_hdrs only when NOT all_headers.
    #    Again, wrap the potentially-configurable list in a filegroup.
    if private_hdrs:
        priv_wrap = name + "_private_hdrs_wrap"
        native.filegroup(
            name = priv_wrap,
            srcs = private_hdrs,
            visibility = ["//visibility:private"],
        )
        extra_srcs = select({
            "//bazel/config:all_headers_enabled": [],
            "//conditions:default": [":" + priv_wrap],
        })
    else:
        extra_srcs = []

    final_srcs = srcs + extra_srcs
    return final_hdrs, final_srcs

def binary_srcs_with_all_headers(name, srcs, private_hdrs):
    pkg = native.package_name()
    if _is_third_party_pkg(pkg) or not pkg.startswith("src/mongo"):
        return srcs + private_hdrs
    if not (pkg.startswith("src/mongo") or "third_party" in pkg):
        return srcs + private_hdrs

    # Always include the config header via srcs
    mongo_cfg_hdr = ["//src/mongo:mongo_config_header"]

    # Wrap private_hdrs so any select(...) inside is contained.
    if private_hdrs:
        priv_wrap = name + "_private_hdrs_wrap"
        native.filegroup(
            name = priv_wrap,
            srcs = private_hdrs,
            visibility = ["//visibility:private"],
        )
        maybe_priv = select({
            "//bazel/config:all_headers_enabled": [],
            "//conditions:default": [":" + priv_wrap],
        })
    else:
        maybe_priv = []

    # Add the per-package all_headers only when all_headers mode is on.
    # Both branches are plain lists → no nested selects.
    all_hdrs_branch = select({
        "//bazel/config:all_headers_enabled": _all_headers_label_for_pkg(pkg),
        "//conditions:default": [],
    })

    return srcs + mongo_cfg_hdr + maybe_priv + all_hdrs_branch

def concat_selects(base_list, select_objs):
    out = base_list or []
    for sel in (select_objs or []):
        out = out + sel
    return out

def strings_only(xs):
    out = []
    for x in (xs or []):
        if type(x) == "string":
            out.append(x)
    return out

def dedupe_preserve_order(items):
    seen = {}
    out = []
    for x in (items or []):
        k = x if type(x) == "string" else str(x)
        if k not in seen:
            seen[k] = True
            out.append(x)
    return out

def filter_srcs_by_auto_headers(strings_srcs, ah_labels):
    """Return strings_srcs minus any file that has a corresponding label in ah_labels.

    strings_srcs: list of strings (no selects)
    ah_labels:    list of auto-header labels, e.g. //pkg/.auto_header:foo_cpp
    """
    if not strings_srcs:
        return []

    # Fast lookup for auto-header labels
    has_ah = {}
    for l in (ah_labels or []):
        has_ah[l] = True

    out = []
    for s in strings_srcs:
        if type(s) != "string":
            continue  # safety

        pkg, name = _split_label_or_file(s)

        # Compute the precise auto-header label we would have made for this path
        ah_label = _auto_header_label(pkg, name)
        if has_ah.get(ah_label, False):
            # An auto-header for this file is present -> drop the raw file to avoid duplicates
            continue
        out.append(s)
    return out

def build_selects_and_flat_files(srcs_select, *, lib_name, debug = False):
    if not srcs_select:
        return [], []
    select_objs = []
    flat_files = []
    for i, condmap in enumerate(srcs_select):
        if type(condmap) != type({}):
            fail("mongo_cc macro({}): srcs_select[{}] must be a dict of {cond: [srcs]}."
                .format(lib_name, i))
        if debug:
            print("mongo_cc macro({}): srcs_select[{}] has {} conditions"
                .format(lib_name, i, len(condmap)))
        for cond, src_list in condmap.items():
            if type(src_list) != type([]):
                fail("mongo_cc macro({}): srcs_select[{}][{}] must be a list of strings"
                    .format(lib_name, i, cond))
            for s in src_list:
                if type(s) != "string":
                    fail("mongo_cc macro({}): srcs_select[{}][{}] item must be string, got {}"
                        .format(lib_name, i, cond, type(s)))
            flat_files.extend(src_list)
        select_objs.append(select(condmap))
    return select_objs, dedupe_preserve_order(flat_files)
