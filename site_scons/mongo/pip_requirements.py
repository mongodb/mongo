# -*- mode: python; -*-

# Try to keep this modules imports minimum and only
# import python standard modules, because this module
# should be used for finding such external modules or
# missing dependencies.
import sys


class MissingRequirements(Exception):
    """Raised when when verify_requirements() detects missing requirements."""
    pass


def verify_requirements(requirements_file: str, silent: bool = False):
    """Check if the modules in a pip requirements file are installed.
    This allows for a more friendly user message with guidance on how to
    resolve the missing dependencies.
    Args:
        requirements_file: path to a pip requirements file.
        silent: True if the function should print.
    Raises:
        MissingRequirements if any requirements are missing
    """

    def verbose(*args, **kwargs):
        if not silent:
            print(*args, **kwargs)

    def raiseSuggestion(ex, pip_pkg):
        raise MissingRequirements(
            f"{ex}\n"
            f"Try running:\n"
            f"    {sys.executable} -m pip install {pip_pkg}"
        ) from ex

    # Import the prequisites for this function, providing hints on failure.
    try:
        import requirements
    except ModuleNotFoundError as ex:
        raiseSuggestion(ex, "requirements_parser")

    try:
        import pkg_resources
    except ModuleNotFoundError as ex:
        raiseSuggestion(ex, "setuptools")

    verbose("Checking required python packages...")

    # Reduce a pip requirements file to its PEP 508 requirement specifiers.
    with open(requirements_file) as fd:
        pip_lines = [p.line for p in requirements.parse(fd)]

    # The PEP 508 requirement specifiers can be parsed by the `pkg_resources`.
    pkg_requirements = list(pkg_resources.parse_requirements(pip_lines))

    verbose("Requirements list:")
    for req in sorted(set([str(req) for req in pkg_requirements])):
        verbose(f"    {req}")

    # Resolve all the requirements at once.
    # This should help expose dependency hell among the requirements.
    try:
        dists = pkg_resources.working_set.resolve(pkg_requirements)
    except pkg_resources.ResolutionError as ex:
        raiseSuggestion(
            ex,
            f"-r {requirements_file}")


    verbose("Resolved to these distributions:")
    for dist in sorted(set([f"    {dist.key} {dist.version}" for dist in dists])):
        verbose(dist)
