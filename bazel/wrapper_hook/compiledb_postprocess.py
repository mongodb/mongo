import json
import os
import pathlib
import tempfile
from urllib.parse import urlparse, unquote


COMPILE_COMMAND_FRAGMENT_EXT = ".compile_command.json"


def _path_for_open(path):
    normalized = os.path.normpath(path)
    if os.name != "nt":
        return normalized

    # Bazel fragment paths can exceed MAX_PATH on Windows even when the build succeeds.
    # Use the extended-length path prefix so Python can open them reliably.
    if normalized.startswith("\\\\?\\"):
        return normalized
    if not os.path.isabs(normalized):
        return normalized
    if normalized.startswith("\\\\"):
        return "\\\\?\\UNC\\" + normalized[2:]
    return "\\\\?\\" + normalized


def _bep_file_path(file_entry):
    if "pathPrefix" in file_entry and "name" in file_entry:
        return os.path.normpath(
            os.path.join(*(file_entry.get("pathPrefix", []) + [file_entry["name"]]))
        )

    uri = file_entry.get("uri")
    if not uri:
        return None

    parsed = urlparse(uri)
    if parsed.scheme != "file":
        return None
    path = unquote(parsed.path)
    if os.name == "nt" and len(path) >= 3 and path[0] == "/" and path[2] == ":":
        path = path[1:]
    return os.path.normpath(path)


def collect_compile_command_fragments(build_event_json):
    fragment_paths = set()
    with open(_path_for_open(build_event_json), "r", encoding="utf-8") as events:
        for line in events:
            if not line.strip():
                continue
            event = json.loads(line)
            named_set = event.get("namedSetOfFiles")
            if not named_set:
                continue
            for file_entry in named_set.get("files", []):
                path = _bep_file_path(file_entry)
                if path and path.endswith(COMPILE_COMMAND_FRAGMENT_EXT):
                    fragment_paths.add(path)

    return sorted(fragment_paths)


def _resolve_fragment_path(fragment_path, output_base=None):
    path = pathlib.Path(fragment_path)
    if path.is_absolute() or path.exists():
        return str(path)
    if output_base is None:
        return str(path)

    output_base_path = pathlib.Path(output_base)
    if fragment_path.startswith("external/"):
        return str(output_base_path / fragment_path)
    return str(output_base_path / "execroot" / "_main" / fragment_path)


def collect_compile_command_fragments_from_roots(search_roots):
    fragment_paths = set()
    for root in search_roots:
        root_path = pathlib.Path(root)
        if not root_path.exists():
            continue
        for fragment in root_path.rglob(f"*{COMPILE_COMMAND_FRAGMENT_EXT}"):
            fragment_paths.add(str(fragment))
    return sorted(fragment_paths)


def load_compile_command_fragments(build_event_json, search_roots=None, output_base=None):
    fragment_paths = collect_compile_command_fragments(build_event_json)
    if not fragment_paths and search_roots:
        fragment_paths = collect_compile_command_fragments_from_roots(search_roots)

    entries = []
    for fragment in fragment_paths:
        resolved_fragment = _resolve_fragment_path(fragment, output_base=output_base)
        with open(_path_for_open(resolved_fragment), "r", encoding="utf-8") as infile:
            fragment_data = json.load(infile)
            if isinstance(fragment_data, list):
                entries.extend(fragment_data)
            else:
                entries.append(fragment_data)
    return entries


def load_compile_command_fragments_from_paths(fragment_paths):
    entries = []
    for fragment in sorted(fragment_paths):
        with open(_path_for_open(fragment), "r", encoding="utf-8") as infile:
            fragment_data = json.load(infile)
            if isinstance(fragment_data, list):
                entries.extend(fragment_data)
            else:
                entries.append(fragment_data)
    return entries


def _entry_key(entry):
    return (
        entry.get("file", ""),
        entry.get("output", ""),
        entry.get("target", ""),
    )


def compile_command_sort_key(entry):
    return (
        entry.get("file", ""),
        entry.get("output", ""),
        entry.get("target", ""),
        entry.get("arguments", []),
    )


def merge_compile_commands(existing_entries, new_entries):
    updated_targets = {
        entry.get("target")
        for entry in new_entries
        if isinstance(entry.get("target"), str) and entry.get("target")
    }
    new_keys = {_entry_key(entry) for entry in new_entries}
    new_file_output_keys = {
        (
            entry.get("file", ""),
            entry.get("output", ""),
        )
        for entry in new_entries
    }

    merged = []
    for entry in existing_entries:
        if _entry_key(entry) in new_keys:
            continue
        if (
            entry.get("file", ""),
            entry.get("output", ""),
        ) in new_file_output_keys:
            continue
        if updated_targets and entry.get("target") in updated_targets:
            continue
        merged.append(entry)

    merged.extend(new_entries)
    merged.sort(key=compile_command_sort_key)
    return merged


def write_compile_commands(entries, output_path):
    output = pathlib.Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    json_str = json.dumps(entries, separators=(",", ":"), ensure_ascii=False)

    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=output.parent,
        delete=False,
    ) as tmp:
        tmp.write(json_str)
        tmp_path = pathlib.Path(tmp.name)

    os.replace(tmp_path, output)
    return True
