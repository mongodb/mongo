import os
import glob
import subprocess
import csv
import io
import enum
import platform
import puremagic
import pathlib
from typing import Optional

from SCons.Node.FS import File, Dir
from typing_extensions import TypedDict
from .util import get_build_metric_dict
from .protocol import BuildMetricsCollector


class ArtifactType(str, enum.Enum):
    UNKNOWN = "unknown"
    PROGRAM = "Program"  # .exe
    LIBRARY = "Library"  # .so, .a
    ARCHIVE = "archive"  # .zip, .tgz, not .a
    OBJECT = "Object"  # .o
    TEXT = "text"  # .h, .hpp, .cpp


# Types to run bloaty against
ARTIFACT_BIN_TYPES = [ArtifactType.PROGRAM, ArtifactType.LIBRARY, ArtifactType.OBJECT]


class BinSize(TypedDict):
    vmsize: int
    filesize: int


class BinMetrics(TypedDict, total=False):
    text: BinSize
    data: BinSize
    rodata: BinSize
    bss: BinSize
    debug: BinSize
    symtab: BinSize
    dyntab: BinSize


def _run_bloaty(bloaty, target) -> Optional[BinMetrics]:
    out = BinMetrics()
    try:
        # -n 0 -> do not collapse small sections into a section named [Other]
        # --csv -> generate csv output to stdout
        # -d sections -> only list sections, not symbols
        proc = subprocess.run([bloaty, "-n", "0", "--csv", "-d", "sections",
                               str(target)], capture_output=True, universal_newlines=True)
        if proc.returncode != 0:
            # if we run bloaty against a thin archive, it will fail. Detect
            # this and allow thin archives to pass, otherwise raise an
            # exception.
            # Note that our thin_archive tool sets the thin_archive
            # attribute to True
            if proc.stderr.startswith("bloaty: unknown file type for file") and getattr(
                    target.attributes, "thin_archive", False):
                # this is a thin archive, pass it
                return None

            raise RuntimeError(f"Failed to call bloaty on '{str(target)}': {proc.stderr}")

        for row in csv.DictReader(proc.stdout.splitlines()):
            # sections,vmsize,filesize
            section = row['sections']
            vmsize = int(row['vmsize'])
            filesize = int(row['filesize'])
            binsize = BinSize(vmsize=vmsize, filesize=filesize)
            if section == ".text":
                out["text"] = binsize
            elif section == ".data":
                out["data"] = binsize
            elif section == ".rodata":
                out["rodata"] = binsize
            elif section == ".bss":
                out["bss"] = binsize
            elif section.startswith(".debug"):
                # there are multiple sections that start with .debug, and we
                # need to sum them up.
                if "debug" not in out:
                    out["debug"] = BinSize(vmsize=0, filesize=0)
                out["debug"]["vmsize"] += vmsize
                out["debug"]["filesize"] += filesize
            elif section == ".symtab":
                out["symtab"] = binsize
            elif section == ".dyntab":
                out["dyntab"] = binsize

        return out

    except FileNotFoundError:
        if not _run_bloaty.printed_missing_bloaty_warning:
            print(
                "WARNING: could not find the bloaty binary. Binary section metrics will not be collected."
            )
            _run_bloaty.printed_missing_bloaty_warning = True
        return None


_run_bloaty.printed_missing_bloaty_warning = False


class Artifact(TypedDict, total=False):
    array_index: int
    name: str
    type: str
    size: int
    bin_metrics: BinMetrics


# First key: platform.system()
# Tuple key 1: ArtifactType
# Tuple Key 2: string to search for
_PLATFORM_LIBMAGIC_BINARY_IDENTITIES = {
    "Windows": [(ArtifactType.LIBRARY, "executable (DLL)"), (ArtifactType.PROGRAM, "executable")],
    "Linux": [(ArtifactType.PROGRAM, "interpreter"), (ArtifactType.LIBRARY, "shared object")],
    "Darwin": [(ArtifactType.PROGRAM, "Mach-O universal binary"),
               (ArtifactType.LIBRARY, "linked shared library")],
}

_ARTIFACT_TYPE_FROM_BUILDER = {
    "SharedObject": ArtifactType.OBJECT,  # .dyn.o
    "StaticObject": ArtifactType.OBJECT,  # .o
    "StaticLibrary": ArtifactType.LIBRARY,  # .a
    "Idlc": ArtifactType.TEXT,  # _gen.{h,cpp}
    "Program": ArtifactType.PROGRAM,  # .exe/*nix binaries
    "Substfile": ArtifactType.TEXT,  # build/opt/mongo/config.h and others
    "InstallBuilder": ArtifactType.TEXT,  # build/opt/third_party/wiredtiger/wiredtiger_ext.h
    "Textfile": ArtifactType.TEXT,  # build/opt/third_party/third_party_shim.cpp
}

_TEXT_IDENTIFIERS = ["ASCII text", "Unicode text"]

_EXTENSION_FALLBACK = {
    ".cpp": ArtifactType.TEXT,
    ".h": ArtifactType.TEXT,
    ".hpp": ArtifactType.TEXT,
    ".js": ArtifactType.TEXT,
    ".idl": ArtifactType.TEXT,
    ".so": ArtifactType.LIBRARY,
    ".o": ArtifactType.OBJECT,

    # Windows
    ".obj": ArtifactType.OBJECT,
    ".lib": ArtifactType.LIBRARY,
    # ilk, exp, pdb and res files on Windows have no appropriate tag, so we
    # allow them to fallthrough to UNKNOWN
}


class CollectArtifacts(BuildMetricsCollector):
    def __init__(self, env):
        self._env = env
        self._env = env
        self._build_dir = env.get("BUILD_METRICS_ARTIFACTS_DIR", env.Dir('#').abspath)
        self._artifacts = []
        self._bloaty_bin = env.get("BUILD_METRICS_BLOATY", env.WhereIs('bloaty'))
        if self._bloaty_bin is None:
            self._bloaty_bin = "bloaty"
        self._metrics = {"total_artifact_size": 0, "num_artifacts": 0, "artifacts": []}

    def get_name(self):
        return "CollectArtifacts"

    def walk(self, dirname):
        for root, dirs, files in os.walk(dirname):
            self._artifacts += list(map(lambda x: os.path.join(root, x), files))

    def finalize(self):
        self.walk(self._env.Dir(self._env.subst(self._build_dir)).path)

        for artifact in self._artifacts:
            artifact_dict = self._identify_artifact(artifact)
            artifact_dict["array_index"] = len(self._metrics["artifacts"])
            self._metrics["artifacts"].append(artifact_dict)
            self._metrics["total_artifact_size"] += artifact_dict["size"]
        self._metrics["num_artifacts"] = len(self._metrics["artifacts"])
        return "artifact_metrics", self._metrics

    def _identify_artifact(self, file_) -> Artifact:
        def _type_from_builder(builder) -> ArtifactType:
            name = builder.get_name(self._env)
            return _ARTIFACT_TYPE_FROM_BUILDER.get(name, ArtifactType.UNKNOWN)

        type_ = ArtifactType.UNKNOWN
        file_str = str(file_)
        node = self._env.File(file_)
        builder = node.get_builder()
        if builder is not None:
            type_ = _type_from_builder(builder)

        if type_ == ArtifactType.UNKNOWN:
            try:
                magic_out = puremagic.from_file(file_str)
                system = platform.system()
                for search_type in _PLATFORM_LIBMAGIC_BINARY_IDENTITIES.get(system):
                    if search_type[1] in magic_out:
                        type_ = search_type[0]
                        break

                if type_ == ArtifactType.UNKNOWN and any(s in magic_out for s in _TEXT_IDENTIFIERS):
                    type_ = ArtifactType.TEXT
            except (puremagic.main.PureError, ValueError):
                # exception means that puremagic failed to id the filetype. We'll
                # fallback to file extension in this case.
                pass
            if type_ == ArtifactType.UNKNOWN:
                type_ = _EXTENSION_FALLBACK.get(pathlib.Path(file_str).suffix, ArtifactType.UNKNOWN)

        out = Artifact({"name": file_, "type": type_, "size": node.get_size()})

        if type_ in ARTIFACT_BIN_TYPES:
            bin_metrics = _run_bloaty(self._bloaty_bin, node)
            if bin_metrics is not None:
                out["bin_metrics"] = bin_metrics

        return out
