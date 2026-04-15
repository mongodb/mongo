#!/usr/bin/env python3
"""
Utility functions for processing CycloneDX SBOMs

"""

import json
import logging
import os
import re
import urllib.parse

logger = logging.getLogger("generate_sbom")
logger.setLevel(logging.NOTSET)

# ################ PURL Validation ################
REGEX_STR_PURL_OPTIONAL = (  # Optional Version (any chars except ? @ #)
    r"(?:@[^?@#]*)?"
    # Optional Qualifiers (any chars except @ #)
    r"(?:\?[^@#]*)?"
    # Optional Subpath (any chars)
    r"(?:#.*)?$"
)

REGEX_PURL = {
    # deb PURL. https://github.com/package-url/purl-spec/blob/main/types-doc/deb-definition.md
    "deb": re.compile(
        r"^pkg:deb/"  # Scheme and type
        # Namespace (organization/user), letters must be lowercase
        r"(debian|ubuntu)+"
        r"/"
        r"[a-z0-9._-]+" + REGEX_STR_PURL_OPTIONAL  # Name
    ),
    # Generic PURL. https://github.com/package-url/purl-spec/blob/main/types-doc/generic-definition.md
    "generic": re.compile(
        r"^pkg:generic/"  # Scheme and type
        r"([a-zA-Z0-9._-]+/)?"  # Optional namespace segment
        r"[a-zA-Z0-9._-]+" + REGEX_STR_PURL_OPTIONAL  # Name (required)
    ),
    # GitHub PURL. https://github.com/package-url/purl-spec/blob/main/types-doc/github-definition.md
    "github": re.compile(
        r"^pkg:github/"  # Scheme and type
        # Namespace (organization/user), letters must be lowercase
        r"[a-z0-9-]+"
        r"/"
        r"[a-z0-9._-]+" + REGEX_STR_PURL_OPTIONAL  # Name (repository)
    ),
    # PyPI PURL. https://github.com/package-url/purl-spec/blob/main/types-doc/pypi-definition.md
    "pypi": re.compile(
        r"^pkg:pypi/"  # Scheme and type
        r"[a-z0-9_-]+"  # Name, letters must be lowercase, dashes, underscore
        + REGEX_STR_PURL_OPTIONAL
    ),
}

# Metadata SBOM requirements
METADATA_FIELDS_REQUIRED = [
    "type",
    "bom-ref",
    "group",
    "name",
    "version",
    "description",
    "licenses",
    "copyright",
    "externalReferences",
    "scope",
]
METADATA_FIELDS_ONE_OF = [
    ["author", "supplier"],
    ["purl", "cpe"],
]


def add_component_property(component: dict, name: str, value: str) -> None:
    """Add a key/value to to 'properties' in SBOM component"""
    if "properties" not in component:
        component["properties"] = []
    component["properties"].append({"name": name, "value": value})


def check_metadata_sbom(meta_bom: dict) -> None:
    """Run checks on SBOM component metadata for expected fields."""
    for component in meta_bom["components"]:
        for field in METADATA_FIELDS_REQUIRED:
            if field not in component:
                logger.warning(
                    "METADATA: %s is missing required field '%s'.",
                    (component.get("bom-ref") or component.get("name")),
                    field,
                )
        for fields in METADATA_FIELDS_ONE_OF:
            found = False
            for field in fields:
                found = found or field in component
            if not found:
                logger.warning(
                    "METADATA: %s is missing one of fields '%s'.",
                    (component.get("bom-ref") or component.get("name")),
                    fields,
                )


def convert_sbom_to_public(sbom_dict: dict):
    """Remove internal-only properties and components from SBOM"""

    original_components_len = len(sbom_dict["components"])
    # Identify internal components based on evidence occurrence in internal folders
    internal_components = [
        c["bom-ref"]
        for c in sbom_dict["components"]
        if any(
            occurence.get("location", "").startswith("src/third_party/private")
            for occurence in c.get("evidence", {}).get("occurrences", [])
        )
        or any(
            property.get("name", "") == "internal:as-is_component"
            for property in c.get("properties", [])
        )
    ]

    # Remove internal components and any dependencies on them from the SBOM
    sbom_dict["components"] = [
        c for c in sbom_dict["components"] if c["bom-ref"] not in internal_components
    ]
    sbom_dict["dependencies"] = [
        d for d in sbom_dict["dependencies"] if d["ref"] not in internal_components
    ]
    for dependency in sbom_dict["dependencies"]:
        dependency["dependsOn"] = [
            d for d in dependency["dependsOn"] if d not in internal_components
        ]
    logger.info(
        "PUBLIC SBOM: Removed %d internal components",
        original_components_len - len(sbom_dict["components"]),
    )
    # Remove internal properties from public components
    original_properties_len = sum(len(c.get("properties", [])) for c in sbom_dict["components"])
    for component in sbom_dict["components"]:
        if "properties" in component:
            component["properties"] = [
                p
                for p in component.get("properties", [])
                if not p.get("name", "").startswith("internal:")
            ]
    logger.info(
        "PUBLIC SBOM: Removed %d internal properties from public components",
        original_properties_len
        - sum(len(c.get("properties", [])) for c in sbom_dict["components"]),
    )


def is_valid_purl(purl: str) -> bool:
    """Validate a GitHub or Generic PURL"""
    for purl_type, regex in REGEX_PURL.items():
        if regex.match(purl):
            logger.debug(
                "PURL: %s matched PURL type '%s' regex '%s'", purl, purl_type, regex.pattern
            )
            return True
    return False


def read_sbom_json_file(file_path: str) -> dict:
    """Load a JSON SBOM file (schema is not validated)"""
    try:
        with open(file_path, "r", encoding="utf-8") as input_json:
            sbom_json = input_json.read()
        result = json.loads(sbom_json)
        logger.info("SBOM loaded from %s with %d components", file_path, len(result["components"]))
        return result
    except OSError as e:
        logger.error("Error loading SBOM file from %s", file_path)
        logger.error(e)
    except json.JSONDecodeError as e:
        logger.error("Error decoding JSON SBOM file from %s", file_path)
        logger.error(e)


def remove_sbom_component(sbom_dict: dict, component_key: str) -> None:
    """Remove a component from the SBOM by its bom-ref key"""
    sbom_dict["components"] = [
        c for c in sbom_dict["components"] if not c["bom-ref"].startswith(component_key)
    ]
    sbom_dict["dependencies"] = [
        d for d in sbom_dict["dependencies"] if not d["ref"].startswith(component_key)
    ]
    for dependency in sbom_dict["dependencies"]:
        dependency["dependsOn"] = [
            d for d in dependency["dependsOn"] if not d.startswith(component_key)
        ]
    logger.debug("Removed component '%s' from SBOM", component_key)


def set_component_version(
    component: dict, version: str, purl_version: str = None, cpe_version: str = None
) -> None:
    """Update the appropriate version fields in a component from the metadata SBOM"""
    if not purl_version:
        purl_version = version

    if not cpe_version:
        cpe_version = version

    component["bom-ref"] = component["bom-ref"].replace("{{VERSION}}", purl_version)
    component["version"] = component["version"].replace("{{VERSION}}", version)
    if component.get("purl"):
        component["purl"] = component["purl"].replace(
            "{{VERSION}}", urllib.parse.quote(purl_version)
        )
        if not is_valid_purl(component["purl"]):
            logger.warning("PURL: Invalid PURL (%s)", component["purl"])
    if component.get("cpe"):
        component["cpe"] = component["cpe"].replace("{{VERSION}}", cpe_version)


def set_dependency_version(dependencies: list, meta_bom_ref: str, purl_version: str) -> None:
    """Update the appropriate dependency version fields from the metadata SBOM"""
    r = 0
    d = 0
    for dependency in dependencies:
        if "{{VERSION}}" in dependency["ref"] and dependency["ref"] == meta_bom_ref:
            dependency["ref"] = dependency["ref"].replace("{{VERSION}}", purl_version)
            r += 1
        for i in range(len(dependency["dependsOn"])):
            if dependency["dependsOn"][i] == meta_bom_ref:
                dependency["dependsOn"][i] = dependency["dependsOn"][i].replace(
                    "{{VERSION}}", purl_version
                )
                d += 1

    logger.debug(
        "set_dependency_version: '%s' updated %d refs and %d dependsOn", meta_bom_ref, r, d
    )


def add_component_dependsOn(dependencies: list, component_ref: str, depends_on_ref: str) -> None:
    """Add a dependsOn reference to a component in the SBOM dependencies"""
    for dependency in dependencies:
        if dependency["ref"] == component_ref:
            if depends_on_ref not in dependency["dependsOn"]:
                dependency["dependsOn"].append(depends_on_ref)
                logger.debug(
                    "Added dependsOn reference '%s' to component '%s'",
                    depends_on_ref,
                    component_ref,
                )
            else:
                logger.debug(
                    "Component '%s' already has dependsOn reference '%s'",
                    component_ref,
                    depends_on_ref,
                )
            return
    # ref missing from .dependencies[]
    dependencies.append({"ref": component_ref, "dependsOn": [depends_on_ref]})
    logger.debug(
        "Added new dependency ref for component '%s' with dependsOn reference '%s'",
        component_ref,
        depends_on_ref,
    )


def sbom_components_to_dict(sbom: dict, with_version: bool = False) -> dict:
    """Create a dict of SBOM components with a version-less PURL as the key"""
    components = sbom["components"]
    if with_version:
        components_dict = {
            urllib.parse.unquote(component["bom-ref"]): component for component in components
        }
    else:
        components_dict = {
            urllib.parse.unquote(component["bom-ref"]).split("@")[0]: component
            for component in components
        }
    return components_dict


def write_sbom_json_file(sbom_dict: dict, file_path: str) -> None:
    """Save a JSON SBOM file (schema is not validated)"""
    try:
        file_path = os.path.abspath(file_path)
        with open(file_path, "w", encoding="utf-8") as output_json:
            formatted_sbom = json.dumps(sbom_dict, indent=2) + "\n"
            output_json.write(formatted_sbom)
    except OSError as e:
        logger.error("Error writing SBOM file to %s", file_path)
        logger.error(e)
    except TypeError as e:
        logger.error("Error serializing SBOM to JSON for file %s", file_path)
        logger.error(e)
    else:
        logger.info("SBOM file saved to %s", file_path)
