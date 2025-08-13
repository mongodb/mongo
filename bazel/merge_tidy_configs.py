#!/usr/bin/env python3
"""
Merge clang-tidy config files:
- Baseline + zero or more additional config files.
- Checks: concatenated left→right (later entries can disable earlier via -pattern).
- CheckOptions: merged by key (later configs override).
- Other keys: deep-merge (dicts) or last-wins (scalars).

Requires: PyYAML
"""

from __future__ import annotations

import argparse
import pathlib
from typing import Any, Dict, List

import yaml

# --------------------------- YAML helpers ---------------------------

def load_yaml(file_path: str | pathlib.Path) -> Dict[str, Any]:  
    path = pathlib.Path(file_path)  
    if not path.exists():  
        raise SystemExit(f"Error: Config file '{file_path}' not found.")  
    with open(path, "r", encoding="utf-8") as f:  
        return yaml.safe_load(f) or {} 


def split_checks_to_list(value: Any) -> List[str]:
    """Normalize a Checks value (string or list) into a flat list of tokens."""
    if value is None:
        return []
    parts: List[str] = []
    if isinstance(value, str):
        parts = [s.strip() for s in value.split(",")]
    elif isinstance(value, list):
        for item in value:
            parts.extend([s.strip() for s in str(item).split(",")])
    return [s for s in parts if s]


def merge_checks_into_config(target_config: Dict[str, Any],
                             incoming_config: Dict[str, Any]) -> None:
    """Append incoming Checks onto target Checks (string-concatenated)."""
    accumulated = split_checks_to_list(target_config.get("Checks"))
    additions = split_checks_to_list(incoming_config.get("Checks"))
    if additions:
        target_config["Checks"] = ",".join(accumulated + additions)


def check_options_list_to_map(value: Any) -> Dict[str, Any]:
    """Transform CheckOptions list[{key,value}] into a dict[key]=value."""
    out: Dict[str, Any] = {}
    if isinstance(value, list):
        for item in value:
            if isinstance(item, dict) and "key" in item:
                out[item["key"]] = item.get("value")
    return out


def merge_check_options_into_config(target_config: Dict[str, Any],
                                    incoming_config: Dict[str, Any]) -> None:
    """
    Merge CheckOptions so later configs override earlier by 'key'.
    Stores back as list[{key,value}] sorted by key for determinism.
    """
    base = check_options_list_to_map(target_config.get("CheckOptions"))
    override = check_options_list_to_map(incoming_config.get("CheckOptions"))
    if override:
        base.update(override)  # later wins
        target_config["CheckOptions"] = [
            {"key": k, "value": v} for k, v in sorted(base.items())
        ]


def deep_merge_dicts(base: Any, override: Any) -> Any:
    """Generic deep merge for everything except Checks/CheckOptions."""
    if isinstance(base, dict) and isinstance(override, dict):
        result = dict(base)
        for key, o_val in override.items():
            if key in ("Checks", "CheckOptions"):
                # handled by specialized mergers
                continue
            b_val = result.get(key)
            if isinstance(b_val, dict) and isinstance(o_val, dict):
                result[key] = deep_merge_dicts(b_val, o_val)
            else:
                result[key] = o_val
        return result
    return override


# --------------------------- path helpers ---------------------------  
  
def is_ancestor_directory(ancestor: pathlib.Path, descendant: pathlib.Path) -> bool:  
    """  
    True if 'ancestor' is the same as or a parent of 'descendant'.  
    Resolution ensures symlinks and relative parts are normalized.  
    """  
    try:  
        ancestor = ancestor.resolve()  
        descendant = descendant.resolve()  
    except FileNotFoundError:  
        # If either path doesn't exist yet, still resolve purely lexicaly  
        ancestor = ancestor.absolute()  
        descendant = descendant.absolute()  
    return ancestor == descendant or ancestor in descendant.parents  
  
  
def filter_and_sort_config_paths(  
    config_paths: list[str | pathlib.Path],  
    scope_directory: str | None  
) -> list[pathlib.Path]:  
    """  
    Keep only config files whose parent directory is an ancestor  
    of the provided scope directory.  
    Sort shallow → deep so deeper configs apply later and override earlier ones.  
    If scope_directory is None, keep paths in the order given.  
    """  
    config_paths = [pathlib.Path(p) for p in config_paths]  
  
    if not scope_directory:  
        return config_paths  
  
    workspace_root = pathlib.Path.cwd().resolve()  
    scope_abs = (workspace_root / scope_directory).resolve()  
  
    selected: list[tuple[int, pathlib.Path]] = []  
  
    for cfg in config_paths:  
        parent_dir = cfg.parent  
        if is_ancestor_directory(parent_dir, scope_abs):  
            # Depth is number of path components from root  
            selected.append((len(parent_dir.parts), cfg.resolve()))  
  
    # Sort by depth ascending so root-most files merge first  
    selected.sort(key=lambda t: t[0])  
  
    return [cfg for _, cfg in selected]


# --------------------------- main ---------------------------

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True, help="Baseline clang-tidy YAML.")
    parser.add_argument(
        "--config-file",
        dest="config_files",
        action="append",
        default=[],
        help="Additional clang-tidy config file(s). May be repeated.",
    )
    parser.add_argument(
        "--scope-dir",
        help="Repo-relative directory; only config files in its ancestor dirs are merged.",
    )
    parser.add_argument("--out", required=True, help="Output merged YAML path.")
    args = parser.parse_args()

    merged_config: Dict[str, Any] = load_yaml(args.baseline)

    config_paths: List[pathlib.Path] = filter_and_sort_config_paths(
        args.config_files, args.scope_dir
    )

    for config_path in config_paths:
        incoming_config = load_yaml(config_path)

        # clang-tidy special merges first:
        merge_checks_into_config(merged_config, incoming_config)
        merge_check_options_into_config(merged_config, incoming_config)

        # then generic merge:
        merged_config = deep_merge_dicts(merged_config, incoming_config)

    merged_config["Checks"] = ",".join(split_checks_to_list(merged_config.get("Checks"))) 
    output_path = pathlib.Path(args.out)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        yaml.safe_dump(merged_config, f, sort_keys=True)


if __name__ == "__main__":
    main()
