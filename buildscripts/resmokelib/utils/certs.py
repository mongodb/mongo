import os

from buildscripts.resmokelib import config


def _get_x509_basepath():
    return (
        "x509"
        if config.INSTALL_DIR is None or config.INSTALL_DIR == ""
        else os.path.join(config.INSTALL_DIR, "x509")
    )


def expand_x509_paths(options: dict) -> dict:
    """Shallowly replace any references to ${x509ObjDir} in option values with the real directory
    containing x509 certificates for testing."""
    new_options = {}
    for k, v in options.items():
        if type(v) == str and r"${x509ObjDir}" in v:
            new_options[k] = v.replace(r"${x509ObjDir}", _get_x509_basepath())
        else:
            new_options[k] = v
    return new_options
