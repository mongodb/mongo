"""Pre-build generator that derives Bazel labels from resmoke suite YAML selectors.

Parses all suite YAML configs, extracts the selector.roots globs, maps them to
Bazel labels, and writes a .bzl file that resmoke.bzl can load to automatically
populate resmoke_suite_test srcs.

This runs during the wrapper_hook pre-build phase, before Bazel's analysis.
"""

import glob
import os
import re
from pathlib import Path

import yaml

try:
    from yaml import CSafeLoader as _YamlLoader
except ImportError:
    from yaml import SafeLoader as _YamlLoader

# Matches the top-level "selector:" block and its indented body.
_SELECTOR_RE = re.compile(r"^selector:\s*\n((?:[ \t]+.*\n)*)", re.MULTILINE)
# Matches the top-level "test_kind:" value.
_TEST_KIND_RE = re.compile(r"^test_kind:\s*(\S+)", re.MULTILINE)

OUTPUT_FILE = Path("bazel/resmoke/.resmoke_suites_derived.bzl")
RESMOKE_MODULES_FILE = Path("buildscripts/resmokeconfig/resmoke_modules.yml")

# Test kinds that don't enumerate individual source files via selector.roots.
# These test binaries directly (mongos --test, dbtest, etc.) so an empty srcs
# list is intentional — not a derive error.
_NO_ROOTS_TEST_KINDS = frozenset(
    [
        "benchmark_test",
        "cpp_integration_test",
        "cpp_libfuzzer_test",
        "cpp_unit_test",
        "db_test",
        "mongos_test",
        "pretty_printer_test",
        "sleep_test",
    ]
)

# Fixed suite directories (relative to repo root).
# Each entry is (suite_dir, bazel_package, target_prefix) where:
#   - suite_dir: filesystem path to the directory
#   - bazel_package: the Bazel package that exports these files
#   - target_prefix: path prefix within the package (for the label key)
_FIXED_SUITE_DIRS = [
    (
        Path("buildscripts/resmokeconfig/suites"),
        "buildscripts/resmokeconfig",
        "suites",
    ),
    (
        Path("buildscripts/resmokeconfig/matrix_suites/generated_suites"),
        "buildscripts/resmokeconfig",
        "matrix_suites/generated_suites",
    ),
]


def _discover_suite_dirs(repo_root: Path) -> list[tuple[Path, str, str]]:
    """Discover all suite directories including modules.

    Returns list of (suite_dir, bazel_package, target_prefix) tuples.
    """
    dirs = list(_FIXED_SUITE_DIRS)

    # Read resmoke_modules.yml to discover module suite dirs
    modules_file = repo_root / RESMOKE_MODULES_FILE
    if modules_file.exists():
        try:
            with open(modules_file) as fh:
                modules_cfg = yaml.load(fh, Loader=_YamlLoader)
            if modules_cfg and isinstance(modules_cfg, dict):
                for module_cfg in modules_cfg.values():
                    if not isinstance(module_cfg, dict):
                        continue

                    # Module suite_dirs
                    for suite_dir in module_cfg.get("suite_dirs", []):
                        p = Path(suite_dir)
                        # Derive bazel package from parent dir
                        bazel_pkg = p.parent.as_posix()
                        target_prefix = p.name
                        dirs.append((p, bazel_pkg, target_prefix))

                    # Module matrix_suite_dirs (use generated_suites subdir)
                    for matrix_dir in module_cfg.get("matrix_suite_dirs", []):
                        p = Path(matrix_dir) / "generated_suites"
                        bazel_pkg = Path(matrix_dir).parent.as_posix()
                        target_prefix = f"{Path(matrix_dir).name}/generated_suites"
                        dirs.append((p, bazel_pkg, target_prefix))
        except Exception:
            pass

    return dirs


def _has_build_file(dir_path: Path) -> bool:
    """Check if a directory contains a BUILD or BUILD.bazel file."""
    return (dir_path / "BUILD.bazel").exists() or (dir_path / "BUILD").exists()


def _glob_to_labels(pattern: str, repo_root: Path) -> list[str]:
    """Convert a resmoke glob pattern to Bazel labels.

    Handles:
    - dir/*.js -> //dir:all_javascript_files
    - dir/**/*.js -> //dir:all_subpackage_javascript_files
    - dir/file.js -> //dir:file.js
    - src/mongo/db/modules/*/path -> expand * against filesystem
    - Complex patterns (test_*.py, *[aA]uth*.js) -> expand via glob
    """
    # Handle module wildcards by expanding against filesystem first
    if "modules/*/" in pattern:
        expanded: list[str] = []
        module_base = pattern.split("modules/*/")[0] + "modules/"
        module_path = repo_root / module_base
        if module_path.is_dir():
            module_dirs = sorted(
                d for d in module_path.iterdir() if d.is_dir() and not d.name.startswith(".")
            )
            for mod_dir in module_dirs:
                expanded_pattern = pattern.replace("modules/*/", f"modules/{mod_dir.name}/")
                expanded.extend(_glob_to_labels(expanded_pattern, repo_root))
        return expanded

    # Standard patterns: dir/*.ext
    if not _has_complex_wildcards(pattern):
        # dir/*.js
        if pattern.endswith("/*.js") and "**" not in pattern:
            dir_path = Path(pattern).parent.as_posix()
            if _has_build_file(repo_root / dir_path):
                return [f"//{dir_path}:all_javascript_files"]
            return []

        # dir/**/*.js
        if pattern.endswith("**/*.js"):
            base_dir = pattern[: pattern.index("**")].rstrip("/")
            if base_dir and _has_build_file(repo_root / base_dir):
                return [f"//{base_dir}:all_subpackage_javascript_files"]
            return []

    # Literal file path (no wildcards at all)
    if "*" not in pattern and "[" not in pattern and "?" not in pattern:
        p = Path(pattern)
        if _has_build_file(repo_root / p.parent):
            return [f"//{p.parent.as_posix()}:{p.name}"]
        return []

    # Complex or non-standard patterns: expand via filesystem glob
    full_pattern = str(repo_root / pattern)
    matches = sorted(glob.glob(full_pattern, recursive=True))
    labels: list[str] = []
    for match in matches:
        rel = os.path.relpath(match, repo_root)
        p = Path(rel)
        if p.is_file():
            labels.append(f"//{p.parent.as_posix()}:{p.name}")
        elif p.is_dir():
            labels.append(f"//{p.as_posix()}")
    return labels


def _has_complex_wildcards(pattern: str) -> bool:
    """Check if a pattern has complex wildcards beyond simple dir/*.ext or dir/**/*.ext."""
    if "[" in pattern:
        return True
    filename = Path(pattern).name
    if filename.count("*") > 1:
        return True
    if "*" in filename and filename not in ("*.js", "*.py", "*.json"):
        return True
    return False


def _render_bzl(selectors: dict[str, list[str]]) -> str:
    """Render the selectors dict as a Starlark .bzl file."""
    lines = [
        '"""Auto-generated suite selector data. DO NOT EDIT.',
        "",
        "Generated by bazel/resmoke/derive_suite_selectors.py",
        '"""',
        "",
        "SUITE_SELECTORS = {",
    ]
    for key in sorted(selectors.keys()):
        srcs = selectors[key]
        lines.append(f'    "{key}": [')
        for src in srcs:
            lines.append(f'        "{src}",')
        lines.append("    ],")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def gen_suite_selectors(repo_root: Path) -> dict[str, object]:
    """Main entry point. Parses suite YAMLs and generates the .bzl file.

    Returns a dict with keys: ok (bool), wrote (bool), err (str|None), count (int), warnings (list[str]).
    """
    try:
        output_file = repo_root / OUTPUT_FILE

        suite_dirs = _discover_suite_dirs(repo_root)

        selectors: dict[str, list[str]] = {}
        warnings: list[str] = []

        for suite_dir, bazel_package, target_prefix in suite_dirs:
            abs_dir = repo_root / suite_dir
            if not abs_dir.is_dir():
                continue

            for yml_path in sorted(abs_dir.glob("*.yml")):
                try:
                    text = yml_path.read_text()
                except Exception as e:
                    warnings.append(f"failed to read {yml_path}: {e}")
                    continue

                # Extract and parse only the selector block for speed.
                m = _SELECTOR_RE.search(text)
                if not m:
                    continue
                try:
                    cfg = yaml.load("selector:\n" + m.group(1), Loader=_YamlLoader)
                except yaml.YAMLError as e:
                    warnings.append(f"failed to parse selector in {yml_path}: {e}")
                    continue

                selector = cfg.get("selector")
                if not selector or not isinstance(selector, dict):
                    # Warn if the file looks like it should have a selector but we
                    # couldn't parse one — likely a YAML indentation error.
                    if "roots:" in text or "from_target:" in text:
                        warnings.append(
                            f"suite {yml_path.name} has selector/roots in text but "
                            f"failed to parse — check YAML indentation"
                        )
                    continue

                # Skip suites that use from_target
                if "from_target" in selector:
                    continue

                roots = selector.get("roots")
                if not roots or not isinstance(roots, list):
                    # For known no-roots test kinds, emit an empty entry so
                    # resmoke_suite_test can proceed in passthrough mode.
                    m_kind = _TEST_KIND_RE.search(text)
                    test_kind = m_kind.group(1) if m_kind else None
                    if test_kind in _NO_ROOTS_TEST_KINDS:
                        key = f"//{bazel_package}:{target_prefix}/{yml_path.name}"
                        selectors[key] = []
                    continue

                # Map each root glob to Bazel labels
                srcs: list[str] = []
                for root in roots:
                    if not isinstance(root, str):
                        continue
                    labels = _glob_to_labels(root, repo_root)
                    srcs.extend(labels)

                if srcs:
                    # Key format: "bazel_package:target_prefix/name.yml"
                    key = f"//{bazel_package}:{target_prefix}/{yml_path.name}"
                    selectors[key] = srcs

        # Write the .bzl file
        content = _render_bzl(selectors)
        output_file.parent.mkdir(parents=True, exist_ok=True)

        # Only write if content changed
        wrote = False
        if not output_file.exists() or output_file.read_text() != content:
            output_file.write_text(content)
            wrote = True

        return {
            "ok": True,
            "wrote": wrote,
            "err": None,
            "count": len(selectors),
            "warnings": warnings,
        }

    except Exception as e:
        return {"ok": False, "wrote": False, "err": str(e), "count": 0, "warnings": []}


if __name__ == "__main__":
    import sys
    import time

    repo_root = Path(__file__).parent.parent.parent
    start = time.perf_counter()
    result = gen_suite_selectors(repo_root)
    elapsed = time.perf_counter() - start

    if result["ok"]:
        action = "wrote" if result["wrote"] else "no change"
        print(
            f"Generated suite selectors for {result['count']} suites ({action}) in {elapsed*1000:.1f}ms"
        )
    else:
        print(f"ERROR: {result['err']}", file=sys.stderr)
        sys.exit(1)
