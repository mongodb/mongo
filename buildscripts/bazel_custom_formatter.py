import argparse
import hashlib
import json
import os
import platform
import stat
import subprocess
import sys
import time
import urllib.request
from collections import deque
from pathlib import Path
from typing import Dict, List

from retry import retry

sys.path.append(".")

from buildscripts.install_bazel import install_bazel
from buildscripts.simple_report import make_report, put_report, try_combine_reports

RELEASE_URL = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/"

groups_sort_keys = {
    "first": 1,
    "second": 2,
    "third": 3,
    "fourth": 4,
    "fifth": 5,
    "sixth": 6,
    "seventh": 7,
    "eighth": 8,
}


@retry(tries=3, delay=5)
def _download_with_retry(*args, **kwargs):
    return urllib.request.urlretrieve(*args, **kwargs)


def determine_platform():
    syst = platform.system()
    pltf = None
    if syst == "Darwin":
        pltf = "darwin"
    elif syst == "Windows":
        pltf = "windows"
    elif syst == "Linux":
        pltf = "linux"
    else:
        raise RuntimeError("Platform cannot be inferred.")
    return pltf


def determine_architecture():
    arch = None
    machine = platform.machine()
    if machine in ("AMD64", "x86_64"):
        arch = "amd64"
    elif machine in ("arm", "arm64", "aarch64"):
        arch = "arm64"
    else:
        raise RuntimeError(f"Detected architecture is not supported: {machine}")

    return arch


def download_buildozer(download_location: str = "./"):
    operating_system = determine_platform()
    architechture = determine_architecture()
    if operating_system == "windows" and architechture == "arm64":
        raise RuntimeError("There are no published arm windows releases for buildozer.")

    extension = ".exe" if operating_system == "windows" else ""
    binary_name = f"buildozer-{operating_system}-{architechture}{extension}"
    url = f"{RELEASE_URL}{binary_name}"

    file_location = os.path.join(download_location, f"buildozer{extension}")
    _download_with_retry(url, file_location)
    os.chmod(file_location, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
    return file_location


def find_group(unittest_paths):
    groups = {
        # group1
        "0": "first",
        "1": "first",
        # group2
        "2": "second",
        "3": "second",
        # group3
        "4": "third",
        "5": "third",
        # group4
        "6": "fourth",
        "7": "fourth",
        # group5
        "8": "fifth",
        "9": "fifth",
        # group6
        "a": "sixth",
        "b": "sixth",
        # group7
        "c": "seventh",
        "d": "seventh",
        # group8
        "e": "eighth",
        "f": "eighth",
    }

    group_to_path: Dict[str, List[str]] = {}

    for path in unittest_paths:
        norm_path = path.replace(":", "/").replace("\\", "/")
        if norm_path.startswith("//"):
            norm_path = norm_path[2:]
        if not norm_path.startswith("src/"):
            print(f"ERROR: {path} not relative to mongo repo root")
            sys.exit(1)

        basename = os.path.basename(norm_path)

        if basename.startswith("lib"):
            basename = basename[3:]

        ext = basename.find(".")
        if ext != -1:
            basename = basename[:ext]
        dirname = os.path.dirname(norm_path)

        hash_path = os.path.join(dirname, basename).replace("\\", "/")
        first_char = hashlib.sha256(hash_path.encode()).hexdigest()[0]
        group = groups[first_char]
        if group not in group_to_path:
            group_to_path[group] = []
        group_to_path[group].append(path)

    return json.dumps(group_to_path, indent=4)


def find_multiple_groups(test, groups):
    tagged_groups = []
    for group in groups:
        if test in groups[group]:
            tagged_groups.append(group)
    return tagged_groups


def iter_clang_tidy_files(root: str | Path) -> list[Path]:
    """Return a list of repo-relative Paths to '.clang-tidy' files.
    - Uses os.scandir for speed
    - Does NOT follow symlinks
    """
    root = Path(root).resolve()
    results: list[Path] = []
    stack = deque([root])

    while stack:
        current = stack.pop()
        try:
            with os.scandir(current) as it:
                for entry in it:
                    name = entry.name
                    if entry.is_dir(follow_symlinks=False):
                        stack.append(Path(entry.path))
                    elif entry.is_file(follow_symlinks=False) and name == ".clang-tidy":
                        # repo-relative path
                        results.append(Path(entry.path).resolve().relative_to(root))
        except PermissionError:
            continue
    return results


def validate_clang_tidy_configs(generate_report, fix):
    buildozer = download_buildozer()

    mongo_dir = "src/mongo"

    tidy_files = iter_clang_tidy_files("src/mongo")

    p = subprocess.run(
        [buildozer, "print label srcs", "//:clang_tidy_config_files"],
        capture_output=True,
        text=True,
    )
    tidy_targets = None
    for line in p.stdout.splitlines():
        if line.startswith("//") and line.endswith("]"):
            tokens = line.split("[")
            tidy_targets = tokens[1][:-1].split(" ")
            break
    if tidy_targets is None:
        print(p.stderr)
        raise Exception(f"could not parse tidy config targets from '{p.stdout}'")

    if tidy_targets == [""]:
        tidy_targets = []

    all_targets = []
    for tidy_file in tidy_files:
        tidy_file_target = (
            "//" + os.path.dirname(os.path.join(mongo_dir, tidy_file)) + ":clang_tidy_config"
        )
        all_targets.append(tidy_file_target)

    if all_targets != tidy_targets:
        msg = f"Incorrect clang tidy config targets: {all_targets} != {tidy_targets}"
        print(msg)
        if generate_report:
            report = make_report("//:clang_tidy_config_files", msg, 1)
            try_combine_reports(report)
            put_report(report)

    if fix:
        subprocess.run(
            [buildozer, f"set srcs {' '.join(all_targets)}", "//:clang_tidy_config_files"]
        )


def validate_bazel_groups(generate_report, fix):
    buildozer = download_buildozer()

    bazel_bin = install_bazel(".")

    query_opts = [
        "--implicit_deps=False",
        "--tool_deps=False",
        "--include_aspects=False",
        "--bes_backend=",
        "--bes_results_url=",
    ]

    try:
        start = time.time()
        sys.stdout.write("Query all unittest binaries... ")
        sys.stdout.flush()
        query_proc = subprocess.run(
            [
                bazel_bin,
                "query",
                'kind(extract_debug, attr(tags, "[\[ ]mongo_unittest[,\]]", //src/...))',
            ]
            + query_opts,
            capture_output=True,
            text=True,
            check=True,
        )
        bazel_unittests = query_proc.stdout.splitlines()
        sys.stdout.write("{:0.2f}s\n".format(time.time() - start))
    except subprocess.CalledProcessError as exc:
        print("BAZEL ERROR:")
        print(exc.stdout)
        print(exc.stderr)
        sys.exit(exc.returncode)

    buildozer_update_cmds = []

    groups = json.loads(find_group(bazel_unittests))
    failures = []
    for group in sorted(groups, key=lambda x: groups_sort_keys[x]):
        try:
            start = time.time()
            sys.stdout.write(f"Query all mongo_unittest_{group}_group unittests... ")
            sys.stdout.flush()
            query_proc = subprocess.run(
                [
                    bazel_bin,
                    "query",
                    f'kind(extract_debug, attr(tags, "[\[ ]mongo_unittest_{group}_group[,\]]", //src/...))',
                ]
                + query_opts,
                capture_output=True,
                text=True,
                check=True,
            )
            sys.stdout.write("{:0.2f}s\n".format(time.time() - start))
            group_tests = query_proc.stdout.splitlines()
        except subprocess.CalledProcessError as exc:
            print("BAZEL ERROR:")
            print(exc.stdout)
            print(exc.stderr)
            sys.exit(exc.returncode)

        if groups[group] != group_tests:
            for test in group_tests:
                if test not in bazel_unittests:
                    failures.append(
                        [
                            test + " tag",
                            f"{test} not a 'mongo_unittest' but has 'mongo_unittest_{group}_group' tag.",
                        ]
                    )
                    print(failures[-1][1])
                    if fix:
                        buildozer_update_cmds += [
                            [f"remove tags mongo_unittest_{group}_group", test]
                        ]

            for test in groups[group]:
                if test not in group_tests:
                    failures.append(
                        [test + " tag", f"{test} missing 'mongo_unittest_{group}_group'"]
                    )
                    print(failures[-1][1])
                    if fix:
                        buildozer_update_cmds += [[f"add tags mongo_unittest_{group}_group", test]]

            for test in group_tests:
                if test not in groups[group]:
                    failures.append(
                        [
                            test + " tag",
                            f"{test} is tagged in the wrong group.",
                        ]
                    )
                    print(failures[-1][1])
                    if fix:
                        buildozer_update_cmds += [
                            [f"remove tags mongo_unittest_{group}_group", test]
                        ]

    if fix:
        for cmd in buildozer_update_cmds:
            subprocess.run([buildozer] + cmd)

    if failures:
        for failure in failures:
            if generate_report:
                report = make_report(failure[0], failure[1], 1)
                try_combine_reports(report)
                put_report(report)


def validate_idl_naming(generate_report: bool, fix: bool) -> None:
    """
    Enforce:
      idl_generator(
        name = "<stem>_gen",
        src  = "<stem>.idl" | ":gen_target"  # where gen_target produces exactly one .idl
      )
    Single `bazel query --output=xml`, parse in-process. Also resolves src labels to generators.
    """
    import xml.etree.ElementTree as ET

    bazel_bin = install_bazel(".")
    qopts = [
        "--implicit_deps=False",
        "--tool_deps=False",
        "--include_aspects=False",
        "--bes_backend=",
        "--bes_results_url=",
    ]

    # One narrowed query: only rules created by the idl_generator macro
    try:
        proc = subprocess.run(
            [
                bazel_bin,
                "query",
                "attr(generator_function, idl_generator, //src/...)",
                "--output=xml",
            ]
            + qopts,
            capture_output=True,
            text=True,
            check=True,
        )
    except subprocess.CalledProcessError as exc:
        print("BAZEL ERROR (narrowed xml):")
        print(exc.stdout)
        print(exc.stderr)
        sys.exit(exc.returncode)

    root = ET.fromstring(proc.stdout)
    failures: list[tuple[str, str]] = []

    def _val(rule, kind, attr):
        n = rule.find(f'./{kind}[@name="{attr}"]')
        return n.get("value") if n is not None else None

    # Prepass: map rule label -> outputs so we can resolve src labels that generate an .idl
    outputs_by_rule: dict[str, list[str]] = {}
    for r in root.findall(".//rule"):
        rname = r.get("name")
        if not rname:
            continue
        outs = [n.get("name") for n in r.findall("./rule-output") if n.get("name")]
        outputs_by_rule[rname] = outs

    for rule in root.findall(".//rule"):
        # Already narrowed, but keep the sentinel check cheap
        if _val(rule, "string", "generator_function") != "idl_generator":
            continue

        rlabel = rule.get("name") or ""
        if not (rlabel.startswith("//") and ":" in rlabel):
            failures.append((rlabel or "<unknown>", "Malformed idl_generator rule label"))
            continue
        pkg, name = rlabel[2:].split(":", 1)

        # Resolve src from label/string/srcs list
        src_val = _val(rule, "label", "src") or _val(rule, "string", "src")
        if not src_val:
            srcs_vals = []
            for lst in rule.findall('./list[@name="srcs"]'):
                srcs_vals += [n.get("value") for n in lst.findall("./label") if n.get("value")]
                srcs_vals += [n.get("value") for n in lst.findall("./string") if n.get("value")]
            if len(srcs_vals) == 1:
                src_val = srcs_vals[0]
            else:
                failures.append(
                    (rlabel, f"'src'/'srcs' must have exactly one entry, got: {srcs_vals}")
                )
                continue

        src = src_val.replace("\\", "/")
        src_base: str | None = None

        if src.startswith("//"):
            spkg, sname = src[2:].split(":")
            if spkg != pkg:
                failures.append((rlabel, f"'src' must be in same package '{pkg}', got '{src}'"))
            if sname.endswith(".idl"):
                src_base = os.path.basename(sname)
            else:
                idl_outs = [o for o in outputs_by_rule.get(src, []) if o.endswith(".idl")]
                if len(idl_outs) != 1:
                    failures.append(
                        (
                            rlabel,
                            f"'src' '{src}' must produce exactly one .idl, got: {idl_outs or outputs_by_rule.get(src, [])}",
                        )
                    )
                    continue
                src_base = os.path.basename(idl_outs[0].split(":", 1)[1])

        elif src.startswith(":"):
            sname = src[1:]
            if sname.endswith(".idl"):
                src_base = os.path.basename(sname)
            else:
                abs_label = f"//{pkg}:{sname}"
                idl_outs = [o for o in outputs_by_rule.get(abs_label, []) if o.endswith(".idl")]
                if len(idl_outs) != 1:
                    failures.append(
                        (
                            rlabel,
                            f"'src' '{src}' must produce exactly one .idl, got: {idl_outs or outputs_by_rule.get(abs_label, [])}",
                        )
                    )
                    continue
                src_base = os.path.basename(idl_outs[0].split(":", 1)[1])

        else:
            if src.startswith("../") or "/../" in src:
                failures.append((rlabel, f"'src' must be within package '{pkg}', got '{src}'"))
            src_base = os.path.basename(src)

        if not (src_base and src_base.endswith(".idl")):
            failures.append((rlabel, f"'src' must resolve to a .idl file, got: {src_base or src}"))
            continue

        if not name.endswith("_gen"):
            failures.append((rlabel, "Target name must end with '_gen'"))

        stem_from_name = name[:-4] if name.endswith("_gen") else name
        stem_from_src = src_base[:-4]
        if stem_from_name != stem_from_src:
            failures.append(
                (
                    rlabel,
                    f"Stem mismatch: name '{name}' vs src '{src_base}'. "
                    f"Expected src basename '{stem_from_name}.idl'.",
                )
            )

    if failures:
        for lbl, msg in failures:
            print(f"IDL naming violation: {lbl}: {msg}")
            if generate_report:
                report = make_report(lbl, msg, 1)
                try_combine_reports(report)
                put_report(report)

    # print(time.time() - start)
    if fix and failures:
        sys.exit(1)


def validate_private_headers(generate_report: bool, fix: bool) -> None:
    """
    Fast header linter/fixer using concurrent buildozer reads:
      buildozer print label srcs //<scope>:%<macro>

    - Lints if any header appears anywhere in the printed block (including select()/glob()).
    - Auto-fixes ONLY concrete items in the first [...] (top-level list).
    - Fails the run if a non-concrete header is detected (select()/glob()).
    """
    import re
    import subprocess
    import sys
    from concurrent.futures import ThreadPoolExecutor, as_completed
    from shlex import split as shlex_split

    # ---- Config ----
    HEADER_EXTS = (".h", ".hh", ".hpp", ".hxx")
    HEADER_RE = re.compile(r"\.(h|hh|hpp|hxx)\b")
    PUBLIC_KEEP = {
        "//src/mongo/platform:basic.h",
        "//src/mongo/platform:windows_basic.h",
    }
    SCOPE = "//src/mongo/..."  # limit to your subtree
    MACRO_SELECTORS = [
        "%mongo_cc_library",
        "%mongo_cc_binary",
        "%mongo_cc_unit_test",
        "%mongo_cc_benchmark",
        "%mongo_cc_integration_test",
        "%mongo_cc_fuzzer_test",
        "%mongo_cc_extension_shared_library",
    ]
    SKIP_SUFFIXES = ("_shared_archive", "_hdrs_wrap")
    SKIP_PKG_SUBSTR = "/third_party/"
    # If True, exit(1) whenever a header is found only via select()/glob()
    FAIL_ON_STRUCTURED = True

    buildozer = download_buildozer()

    def _run_print(selector: str) -> tuple[str, str]:
        """Run one buildozer print invocation; return (selector, stdout)."""
        try:
            out = subprocess.run(
                [buildozer, "print label srcs", f"{SCOPE}:{selector}"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            return selector, out
        except subprocess.CalledProcessError as exc:
            # surface error and keep going (treated as empty output)
            print(f"BUILDOZER ERROR (print label srcs) for selector {selector}:", file=sys.stderr)
            print(exc.stdout, file=sys.stderr)
            print(exc.stderr, file=sys.stderr)
            return selector, ""

    # 1) Run all macro prints concurrently
    outputs: list[str] = []
    with ThreadPoolExecutor(max_workers=min(4, max(1, len(MACRO_SELECTORS)))) as ex:
        futs = [ex.submit(_run_print, sel) for sel in MACRO_SELECTORS]
        for fut in as_completed(futs):
            _, stdout = fut.result()
            if stdout:
                outputs.append(stdout)

    if not outputs:
        return

    combined = "\n".join(outputs)

    # 2) Parse into target blocks: start at lines beginning with //src/mongo...
    target_line_re = re.compile(r"^//src/mongo/[^:\s\[]+:[^\s\[]+")
    lines = combined.splitlines()
    blocks: list[tuple[str, list[str]]] = []
    cur_target: str | None = None
    cur_buf: list[str] = []

    def flush():
        nonlocal cur_target, cur_buf
        if cur_target is not None:
            blocks.append((cur_target, cur_buf))
        cur_target, cur_buf = None, []

    for line in lines:
        if target_line_re.match(line):
            flush()
            cur_target = line.split()[0]
            cur_buf = [line]
        elif cur_target is not None:
            cur_buf.append(line)
    flush()

    failures: list[tuple[str, str]] = []
    fixes: list[tuple[str, str]] = []  # (cmd, target)
    structured_fail_found = False  # to enforce FAIL_ON_STRUCTURED

    def pkg_of(label: str) -> str:
        return label[2:].split(":", 1)[0]

    def normalize_token(pkg: str, tok: str) -> str | None:
        t = tok.strip().strip(",")
        if not t:
            return None
        if t.startswith(("select(", "glob(")):
            return None
        if t.startswith("//"):
            return t
        if t.startswith(":"):
            return f"//{pkg}:{t[1:]}"
        # bare filename/path → pkg-local
        if not any(ch in t for ch in " []{}:\t\n"):
            return f"//{pkg}:{t}"
        return None

    for target, buf in blocks:
        if target.endswith(SKIP_SUFFIXES) or SKIP_PKG_SUBSTR in target:
            continue

        text = "\n".join(buf)

        # quick lint: any .h* anywhere?
        if not HEADER_RE.search(text):
            continue

        # first [...] only (top-level list)
        m = re.search(r"\[(.*?)\]", text, flags=re.DOTALL)
        top_tokens: list[str] = []
        if m:
            inner = m.group(1).replace("\n", " ").strip()
            if inner:
                try:
                    top_tokens = shlex_split(inner)
                except ValueError:
                    top_tokens = inner.split()

        pkg = pkg_of(target)
        concrete_headers: list[str] = []
        for tok in top_tokens:
            norm = normalize_token(pkg, tok)
            if not norm:
                continue
            if norm in PUBLIC_KEEP:
                continue
            base = norm.split(":", 1)[1]
            if base.endswith(HEADER_EXTS):
                concrete_headers.append(norm)

        structured_has_hdr = False
        if not concrete_headers:
            # If there were headers somewhere but none in first [...], we assume select()/glob()
            structured_has_hdr = True

        if not concrete_headers and not structured_has_hdr:
            continue

        canon_target = target.replace("_with_debug", "")

        parts = []
        if concrete_headers:
            parts.append(f"concrete headers: {concrete_headers}")
        if structured_has_hdr:
            parts.append("headers via select()/glob() (not auto-fixed)")
            structured_fail_found = True

        msg = f"{canon_target} has headers in srcs: " + "; ".join(parts)
        print(msg)
        failures.append((canon_target, msg))

        if fix and concrete_headers:
            for h in concrete_headers:
                fixes.append((f"add private_hdrs {h}", canon_target))
                fixes.append((f"remove srcs {h}", canon_target))

    # 3) Apply fixes (dedupe)
    if fix and fixes:
        seen = set()
        for cmd, tgt in fixes:
            key = (cmd, tgt)
            if key in seen:
                continue
            seen.add(key)
            subprocess.run([buildozer, cmd, tgt])

    # 4) CI reports
    if failures and generate_report:
        for tlabel, msg in failures:
            report = make_report(tlabel, msg, 1)
            try_combine_reports(report)
            put_report(report)

    # 5) Failing rules
    # - Always fail if any violation and not fixing (your existing behavior)
    # - Also fail if we saw non-concrete (structured) headers anywhere (requested)
    if (failures and not fix) or (structured_fail_found and FAIL_ON_STRUCTURED):
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument("--generate-report", default=False, action="store_true")
    parser.add_argument("--fix", default=False, action="store_true")
    args = parser.parse_args()
    validate_clang_tidy_configs(args.generate_report, args.fix)
    validate_bazel_groups(args.generate_report, args.fix)
    validate_idl_naming(args.generate_report, args.fix)
    validate_private_headers(args.generate_report, args.fix)


if __name__ == "__main__":
    main()
