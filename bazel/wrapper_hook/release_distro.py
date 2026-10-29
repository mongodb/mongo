"""Require Evergreen release builds to execute on release distros."""

import argparse
import os
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

from bazel.wrapper_hook.hermetic_container.bazelrc import _effective_config_values
from bazel.wrapper_hook.hermetic_container.constants import REPO_ROOT

RELEASE_CONFIGS = ("public-release", "public-release-local", "public-release-rbe")


class ReleaseDistroError(ValueError):
    """An unsafe release invocation that must not use the wrapper's CI fallback."""


def validate_release_distro_id(distro_id: str | None) -> None:
    """Validate the actual executing Evergreen distro, using its release- convention."""
    if (
        not isinstance(distro_id, str)
        or not distro_id.startswith("release-")
        or len(distro_id) == 8
    ):
        raise ReleaseDistroError(
            f"Evergreen release build requires a release-* distro; distro_id={distro_id or '<missing>'!r}. "
            "Assign the task or task group to a suitable release distro."
        )


def check_evergreen_release_distro(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    repo_root: Path = REPO_ROOT,
    *,
    evergreen: bool = False,
) -> tuple[str, ...]:
    """Check selected release profiles, including RC defaults and aliases.

    The Evergreen shell wrapper supplies evergreen=True, so an absent distro expansion
    cannot turn off enforcement. Other callers retain ordinary local build behavior.
    """
    if not evergreen and not any(key in env for key in ("task_id", "EVG_TASK_ID", "distro_id")):
        return ()
    configs = _effective_config_values(args, env=env, repo_root=repo_root)
    profiles = tuple(dict.fromkeys(config for config in configs if config in RELEASE_CONFIGS))
    if profiles:
        try:
            validate_release_distro_id(env.get("distro_id"))
        except ValueError as exc:
            raise ReleaseDistroError(
                f"Release configuration(s) {', '.join(profiles)}: {exc}"
            ) from exc
        print(
            f"Release configuration(s) {', '.join(profiles)}: distro_id={env['distro_id']}",
            file=sys.stderr,
        )
    return profiles


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evergreen", action="store_true")
    parser.add_argument("args", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    args = options.args[1:] if options.args[:1] == ["--"] else options.args
    try:
        check_evergreen_release_distro(args, repo_root=Path.cwd(), evergreen=options.evergreen)
    except ValueError as exc:
        parser.exit(1, f"ERROR: {exc}\n")


if __name__ == "__main__":
    main()
