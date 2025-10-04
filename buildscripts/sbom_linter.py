import argparse
import json
import os
import sys
from typing import List

import jsonschema
from license_expression import get_spdx_licensing
from referencing import Registry, Resource

BOM_SCHEMA_LOCATION = os.path.join("buildscripts", "tests", "sbom_linter", "bom-1.5.schema.json")
SPDX_SCHEMA_LOCATION = os.path.join("buildscripts", "tests", "sbom_linter", "spdx.schema.json")
SPDX_SCHEMA_REF = "spdx.schema.json"

# directory to scan for third party libraries
THIRD_PARTY_DIR = os.path.join("src", "third_party")
# platform independent prefix of third party libraries
THIRD_PARTY_LOCATION_PREFIX = "src/third_party/"
# This should only be set to true for testing to ensure the tests to not rely on the current state
# of the third party library dir.
SKIP_FILE_CHECKING = False

# Error messages used for matching in testing
UNDEFINED_THIRD_PARTY_ERROR = (
    "The following files in src/third_party do not have components defined in the sbom:"
)
FORMATTING_ERROR = "File has incorrect formatting, re-run `buildscripts/sbom_linter.py` with the `--format` option to fix this."
MISSING_PURL_CPE_ERROR = "Component must include a 'purl' or 'cpe' field."
MISSING_EVIDENCE_ERROR = (
    "Component must include an 'evidence.occurrences' field when the scope is required."
)
MISSING_TEAM_ERROR = "Component must include a 'internal:team_responsible' property."
SCHEMA_MATCH_FAILURE = "File did not match the CycloneDX schema"
MISSING_VERSION_IN_SBOM_COMPONENT_ERROR = "Component must include a version."
MISSING_VERSION_IN_IMPORT_FILE_ERROR = "Missing version in the import file: "
MISSING_LICENSE_IN_SBOM_COMPONENT_ERROR = "Component must include a license."
COULD_NOT_FIND_OR_READ_SCRIPT_FILE_ERROR = "Could not find or read the import script file"
VERSION_MISMATCH_ERROR = "Version mismatch: "


# A class for managing error messages for components
class ErrorManager:
    def __init__(self, input_file: str):
        self.input_file: str = input_file
        self.component_name: str = ""
        self.errors: List[str] = []

    def update_component_attribute(self, component_name: str) -> None:
        self.component_name = component_name

    def append(self, message: str) -> None:
        self.errors.append(message)

    def append_full_error_message(self, message: str) -> None:
        if self.component_name == "":
            self.errors.append(f"Input-file:{self.input_file} Error: {message}")
        else:
            self.errors.append(
                f"Input-file:{self.input_file} Component:{self.component_name} Error: {message}"
            )

    def print_errors(self) -> None:
        if self.errors:
            print("\n".join(self.errors), file=sys.stderr)

    def zero_error(self) -> bool:
        return bool(not self.errors)

    def find_message_in_errors(self, message: str) -> bool:
        message_found = False
        for error in self.errors:
            if message in error:
                message_found = True
                break
        return message_found


def get_schema() -> dict:
    with open(BOM_SCHEMA_LOCATION, "r") as schema_data:
        return json.load(schema_data)


def local_schema_registry():
    "Create a local registry which is used to resolve references to external schema"

    with open(SPDX_SCHEMA_LOCATION, "r") as spdx:
        spdx_schema = Resource.from_contents(json.load(spdx))
    return Registry().with_resources([(SPDX_SCHEMA_REF, spdx_schema)])


# The script_path is a file which may contain a line where two tokens script_version_key and
# the version string are separated by a separtor value of "=" and some optional spaces.
# There is an "end of line" delimiter at the end of the line which needs to be stripped.
def get_script_version(
    script_path: str, script_version_key: str, error_manager: ErrorManager
) -> str:
    result = ""
    try:
        file = open(script_path, "r")
    except OSError:
        error_manager.append_full_error_message(COULD_NOT_FIND_OR_READ_SCRIPT_FILE_ERROR)
        return result

    with file:
        for line in file:
            # Remove possible spaces, string delimiters and an "end of line" delimiter.
            tokens = line.rstrip().replace('"', "").replace(" ", "").split("=")
            if (len(tokens) > 1) and (tokens[0] == script_version_key):
                result = tokens[1]
                break
    return result


# A version string sometimes contains an extra prefix like "v1.2" instead of "1.2"
# This function strips that extra prefix.
def strip_extra_prefixes(string_with_prefix: str) -> str:
    return string_with_prefix.removeprefix("mongo/").removeprefix("v")


def validate_license(component: dict, error_manager: ErrorManager) -> None:
    if "licenses" not in component:
        error_manager.append_full_error_message(MISSING_LICENSE_IN_SBOM_COMPONENT_ERROR)
        return

    valid_license = False
    for license in component["licenses"]:
        if "expression" in license:
            expression = license.get("expression")
        elif "license" in license:
            if "id" in license["license"]:
                # Should be a valid SPDX license ID
                expression = license["license"].get("id")
            elif "name" in license["license"]:
                # If SPDX does not define the license used, the name field may be used to provide the license name
                valid_license = True

        if not valid_license:
            licensing_validate = get_spdx_licensing().validate(expression, validate=True)
            # ExpressionInfo(
            #   original_expression='',
            #   normalized_expression='',
            #   errors=[],
            #   invalid_symbols=[]
            # )
            valid_license = not licensing_validate.errors or not licensing_validate.invalid_symbols
            if not valid_license:
                error_manager.append_full_error_message(licensing_validate)
                return


def validate_evidence(component: dict, third_party_libs: set, error_manager: ErrorManager) -> None:
    if component["scope"] == "required":
        if "evidence" not in component or "occurrences" not in component["evidence"]:
            error_manager.append_full_error_message(MISSING_EVIDENCE_ERROR)
            return

    validate_location(component, third_party_libs, error_manager)


def validate_properties(component: dict, error_manager: ErrorManager) -> None:
    has_team_responsible_property = False or component["scope"] == "excluded"
    script_path = ""
    if "properties" in component:
        for prop in component["properties"]:
            if prop["name"] == "internal:team_responsible":
                has_team_responsible_property = True
            elif prop["name"] == "import_script_path":
                script_path = prop["value"]
    if not has_team_responsible_property:
        error_manager.append_full_error_message(MISSING_TEAM_ERROR)

    if not component.get("version"):
        error_manager.append_full_error_message(MISSING_VERSION_IN_SBOM_COMPONENT_ERROR)
        return

    comp_version = component["version"]
    # If the version is unknown or the script path property is absent, the version
    # check is not possible (these are valid options and no error is generated).
    if comp_version == "Unknown" or script_path == "":
        return

    # Include the .pedigree.descendants[0] version for version matching
    if (
        "pedigree" in component
        and "descendants" in component["pedigree"]
        and "version" in component["pedigree"]["descendants"][0]
    ):
        comp_pedigree_version = component["pedigree"]["descendants"][0]["version"]
    else:
        comp_pedigree_version = ""

    # At this point a version is attempted to be read from the import script file
    script_version = get_script_version(script_path, "VERSION", error_manager)
    if script_version == "":
        error_manager.append_full_error_message(MISSING_VERSION_IN_IMPORT_FILE_ERROR + script_path)
    elif strip_extra_prefixes(script_version) != strip_extra_prefixes(
        comp_version
    ) and strip_extra_prefixes(script_version) != strip_extra_prefixes(comp_pedigree_version):
        error_manager.append_full_error_message(
            VERSION_MISMATCH_ERROR
            + f"\nscript version:{script_version}\nsbom component version:{comp_version}\nsbom component pedigree version:{comp_pedigree_version}"
        )


def validate_component(component: dict, third_party_libs: set, error_manager: ErrorManager) -> None:
    error_manager.update_component_attribute(component["name"])
    if "scope" not in component:
        error_manager.append_full_error_message("component must include a scope.")
    else:
        validate_evidence(component, third_party_libs, error_manager)
    validate_properties(component, error_manager)
    validate_license(component, error_manager)

    if "purl" not in component and "cpe" not in component:
        error_manager.append_full_error_message(MISSING_PURL_CPE_ERROR)
    error_manager.update_component_attribute("")


def validate_location(component: dict, third_party_libs: set, error_manager: ErrorManager) -> None:
    if "evidence" in component:
        if "occurrences" not in component["evidence"]:
            error_manager.append_full_error_message(
                "'evidence.occurrences' field must include at least one location."
            )

        occurrences = component["evidence"]["occurrences"]
        for occurrence in occurrences:
            if "location" in occurrence:
                location = occurrence["location"]

                if not os.path.exists(location) and not SKIP_FILE_CHECKING:
                    error_manager.append_full_error_message("location does not exist in repo.")

                if location.startswith(THIRD_PARTY_LOCATION_PREFIX):
                    lib = location.removeprefix(THIRD_PARTY_LOCATION_PREFIX)
                    if lib in third_party_libs:
                        third_party_libs.remove(lib)


def lint_sbom(
    input_file: str, output_file: str, third_party_libs: set, should_format: bool
) -> ErrorManager:
    with open(input_file, "r", encoding="utf-8") as sbom_file:
        sbom_text = sbom_file.read()

    error_manager = ErrorManager(input_file)

    try:
        sbom = json.loads(sbom_text)
    except Exception as ex:
        error_manager.append(f"Failed to parse {input_file}: {str(ex)}")
        return error_manager

    try:
        schema = get_schema()
        jsonschema.validators.validator_for(schema)(
            schema, registry=local_schema_registry()
        ).validate(sbom)
    except jsonschema.ValidationError as error:
        error_manager.append(f"{SCHEMA_MATCH_FAILURE} {input_file}")
        error_manager.append(error.message)
        return error_manager

    components = sbom["components"]
    for component in components:
        validate_component(component, third_party_libs, error_manager)

    if third_party_libs:
        error_manager.append(UNDEFINED_THIRD_PARTY_ERROR)
        for lib in third_party_libs:
            error_manager.append(f"    {lib}")

    formatted_sbom = json.dumps(sbom, indent=2) + "\n"
    if formatted_sbom != sbom_text:
        error_manager.append(f"{input_file} {FORMATTING_ERROR}")

    if should_format:
        with open(output_file, "w", encoding="utf-8") as sbom_file:
            sbom_file.write(formatted_sbom)
    return error_manager


def main() -> int:
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--format",
        action="store_true",
        default=False,
        help="Whether to apply formatting to the output file.",
    )
    parser.add_argument(
        "--input-file", default="sbom.json", help="The input CycloneDX file to format and lint."
    )
    parser.add_argument(
        "--output-file",
        default="sbom.json",
        help="The file to output to when formatting is specified.",
    )
    args = parser.parse_args()
    should_format = args.format
    input_file = args.input_file
    output_file = args.output_file
    third_party_libs = set(
        [
            path
            for path in os.listdir(THIRD_PARTY_DIR)
            if not os.path.isfile(os.path.join(THIRD_PARTY_DIR, path))
        ]
    )
    # the only files in this dir that are not third party libs
    third_party_libs.remove("scripts")
    # the only files in the sasl dir are BUILD files to setup the sasl library in Windows
    third_party_libs.remove("sasl")
    error_manager = lint_sbom(input_file, output_file, third_party_libs, should_format)
    error_manager.print_errors()

    return 0 if error_manager.zero_error() else 1


if __name__ == "__main__":
    sys.exit(main())
