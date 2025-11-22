#!/usr/bin/env python3
"""Utility functions for the Endor Labs API via endorctl."""

import json
import logging
import subprocess
import time
from datetime import datetime
from enum import Enum

logger = logging.getLogger("generate_sbom")
logger.setLevel(logging.NOTSET)

default_field_masks = {
    "PackageVersion": [
        "context",
        "meta",
        "processing_status",
        "spec.package_name",
        "spec.resolved_dependencies.dependencies",
        "spec.source_code_reference",
    ],
    "ScanResult": [
        "context",
        "meta",
        "spec.end_time",
        "spec.logs",
        "spec.refs",
        "spec.start_time",
        "spec.status",
        "spec.versions",
    ],
}


def _get_default_field_mask(kind):
    default_field_mask = default_field_masks.get(kind, [])
    return ",".join(default_field_mask)


class EndorResourceKind(Enum):
    """Enumeration for Endor Labs API resource kinds."""

    PROJECT = "Project"
    REPOSITORY_VERSION = "RepositoryVersion"
    SCAN_RESULT = "ScanResult"
    PACKAGE_VERSION = "PackageVersion"


class EndorContextType(Enum):
    """
    Most objects include a common nested object called Context. Contexts keep objects from different scans separated.

    https://docs.endorlabs.com/rest-api/using-the-rest-api/data-model/common-fields/#context
    """

    # Objects from a scan of the default branch. All objects in the OSS namespace are in the main context. The context ID is always default.
    MAIN = "CONTEXT_TYPE_MAIN"
    # Objects from a scan of a specific branch. The context ID is the branch reference name.
    REF = "CONTEXT_TYPE_REF"
    # Objects from a PR scan. The context ID is the PR UUID. Objects in this context are deleted after 30 days.
    CI_RUN = "CONTEXT_TYPE_CI_RUN"


class EndorFilter:
    """Provide standard filters for Endor Labs API resource kinds."""

    def __init__(self, context_id=None, context_type=None):
        self.context_id = context_id
        self.context_type = context_type

    def _basefilters(self):
        basefilters = []
        if self.context_id:
            basefilters.append(f"context.id=={self.context_id}")
        if self.context_type:
            basefilters.append(f"context.type=={self.context_type}")

        return basefilters

    def repository_version(self, project_uuid=None, sha=None, ref=None):
        filters = self._basefilters()
        if project_uuid:
            filters.append(f"meta.parent_uuid=={project_uuid}")
        if sha:
            filters.append(f"spec.version.sha=={sha}")
        if ref:
            filters.append(f"spec.version.ref=={ref}")

        return " and ".join(filters)

    def package_version(
            self,
            context_type=None,
            context_id=None,
            project_uuid=None,
            name=None,
            package_name=None,
    ):
        filters = self._basefilters()
        if context_type:
            filters.append(f"context.type=={context_type.value}")
        if context_type:
            filters.append(f"context.id=={context_id}")
        if project_uuid:
            filters.append(f"spec.project_uuid=={project_uuid}")
        if name:
            filters.append(f"spec.package_name=={name}")
        if package_name:
            filters.append(f"meta.name=={package_name}")

        return " and ".join(filters)

    def scan_result(
            self,
            context_type=None,
            project_uuid=None,
            ref=None,
            sha=None,
            status=None,
    ):
        filters = self._basefilters()
        if context_type:
            filters.append(f"context.type=={context_type.value}")
        if project_uuid:
            filters.append(f"meta.parent_uuid=={project_uuid}")
        if ref:
            filters.append(f"spec.versions.ref contains '{ref}'")
        if sha:
            filters.append(f"spec.versions.sha contains '{sha}'")
        if status:
            filters.append(f"spec.status=={status}")

        return " and ".join(filters)


class EndorCtl:
    """Interact with endorctl (Endor Labs CLI)."""

    # region internal functions
    def __init__(
            self,
            namespace,
            retry_limit=5,
            sleep_duration=30,
            endorctl_path="endorctl",
            config_path=None,
    ):
        self.namespace = namespace
        self.retry_limit = retry_limit
        self.sleep_duration = sleep_duration
        self.endorctl_path = endorctl_path
        self.config_path = config_path

    def _call_endorctl(self, command, subcommand, **kwargs) -> dict:
        """https://docs.endorlabs.com/endorctl/ ."""

        result = None
        try:
            command = [self.endorctl_path, command, subcommand, f"--namespace={self.namespace}"]
            if self.config_path:
                command.append(f"--config-path={self.config_path}")

            # parse args into flags
            for key, value in kwargs.items():
                # Handle endorctl flags with hyphens that are defined in the script with underscores
                flag = key.replace("_", "-")
                if value:
                    command.append(f"--{flag}={value}")
            logger.info("Running: %s", " ".join(command))

            result = subprocess.run(command, capture_output=True, text=True, check=True)

            resource = json.loads(result.stdout)

        except subprocess.CalledProcessError as err:
            logger.error("Error executing command: %s", err.cmd)
            logger.error(err.stderr)
        except json.JSONDecodeError as err:
            logger.error("Error decoding JSON: %s", err.msg)
            assert isinstance(result, subprocess.CompletedProcess)
            logger.error("Stdout: %s", result.stdout)
        except FileNotFoundError as err:
            logger.error("FileNotFoundError: %s", err.filename)
            logger.error(
                "'endorctl' not found in path '%s'. Supply the correct path, run 'buildscripts/install_endorctl.sh' or visit https://docs.endorlabs.com/endorctl/install-and-configure/",
                self.endorctl_path)
        except Exception as err:
            logger.error("An unexpected error occurred: %s", str(err))
        else:
            return resource
        return {}

    def _api_get(self, resource, **kwargs):
        """https://docs.endorlabs.com/endorctl/commands/api/ ."""
        return self._call_endorctl("api", "get", resource=resource, **kwargs)

    # pylint: disable=W0622 # redefined-builtin
    def _api_list(self, resource, filter=None, retry=True, **kwargs):
        """https://docs.endorlabs.com/endorctl/commands/api/ ."""
        # If this script is run immediately after making a commit, Endor Labs will likely not yet have created the assocaited ScanResult object. The wait/retry logic below handles this scenario.
        tries = 0
        while True:
            tries += 1
            result = self._call_endorctl("api", "list", resource=resource, filter=filter, **kwargs)

            # The expected output of 'endorctl api list' is: { "list": { "objects": [...] } }
            # We want to just return the objects. In case we get an empty list, return a list
            # with a single None to avoid having to handle index errors downstream.
            if result and result["list"].get("objects") and len(result["list"]["objects"]) > 0:
                return result["list"]["objects"]
            elif retry:
                logger.info("API LIST: Resource not found: %s with filter '%s' in namespace '%s'",
                            resource, filter, self.namespace)
                if tries <= self.retry_limit:
                    logger.info("API LIST: Waiting for %s seconds before retry attempt %s of %s",
                                self.sleep_duration, tries, self.retry_limit)
                    time.sleep(self.sleep_duration)
                else:
                    logger.warning(
                        "API LIST: Maximum number of allowed retries %s attempted with no %s found using filter '%s'",
                        self.retry_limit, resource, filter)
                    return [None]
            else:
                return [None]

    def _check_resource(self, resource, resource_description) -> None:
        if not resource:
            raise LookupError(f"Resource not found: {resource_description}")
        logger.info("Retrieved: %s", resource_description)

    # endregion internal functions

    # region resource functions
    def get_resource(self, resource, uuid=None, name=None, field_mask=None, **kwargs):
        """https://docs.endorlabs.com/rest-api/using-the-rest-api/data-model/resource-kinds/ ."""
        if not field_mask:
            field_mask = _get_default_field_mask(resource)
        return self._api_get(resource=resource, uuid=uuid, name=name, field_mask=field_mask,
                             **kwargs)

    def get_resources(
            self,
            resource,
            filter=None,
            field_mask=None,
            sort_path="meta.create_time",
            sort_order="descending",
            retry=True,
            **kwargs,
    ):
        """https://docs.endorlabs.com/rest-api/using-the-rest-api/data-model/resource-kinds/ ."""
        if not field_mask:
            field_mask = _get_default_field_mask(resource)
        return self._api_list(
            resource=resource,
            filter=filter,
            field_mask=field_mask,
            sort_path=sort_path,
            sort_order=sort_order,
            retry=retry,
            **kwargs,
        )

    def get_project(self, git_url):
        resource_kind = EndorResourceKind.PROJECT.value
        resource_description = (
            f"{resource_kind} with name '{git_url}' in namespace '{self.namespace}'")
        project = self.get_resource(resource_kind, name=git_url)
        self._check_resource(project, resource_description)
        return project

    def get_repository_version(self, filter=None, retry=True):
        resource_kind = EndorResourceKind.REPOSITORY_VERSION.value
        resource_description = (
            f"{resource_kind} with filter '{filter}' in namespace '{self.namespace}'")
        repository_version = self.get_resources(resource_kind, filter=filter, retry=retry,
                                                page_size=1)[0]
        self._check_resource(repository_version, resource_description)
        return repository_version

    def get_scan_result(self, filter=None, retry=True):
        resource_kind = EndorResourceKind.SCAN_RESULT.value
        resource_description = (
            f"{resource_kind} with filter '{filter}' in namespace '{self.namespace}'")
        scan_result = self.get_resources(resource_kind, filter=filter, retry=retry, page_size=1)[0]
        self._check_resource(scan_result, resource_description)
        assert scan_result is not None
        uuid = scan_result.get("uuid")
        start_time = scan_result["spec"].get("start_time")
        refs = scan_result["spec"].get("refs")
        polling_start_time = datetime.now()
        while True:
            assert scan_result is not None
            status = scan_result["spec"].get("status")
            end_time = scan_result["spec"].get("end_time")
            if status == "STATUS_SUCCESS":
                logger.info(
                    "   Scan completed successfully. ScanResult uuid %s for refs %s started at %s, ended at %s.",
                    uuid, refs, start_time, end_time)
                return scan_result
            elif status == "STATUS_RUNNING":
                logger.info("   Scan is running. ScanResult uuid %s for refs %s started at %s.",
                            uuid, refs, start_time)
                logger.info(
                    "     Waiting %s seconds before checking status. Total wait time: %s minutes",
                    self.sleep_duration, "{:.2f}".format(
                        (datetime.now() - polling_start_time).total_seconds() / 60))
                time.sleep(self.sleep_duration)
                scan_result = self.get_resources(resource_kind, filter=filter, retry=retry,
                                                 page_size=1)[0]
            elif status == "STATUS_PARTIAL_SUCCESS":
                scan_logs = scan_result["spec"].get("logs")
                raise RuntimeError(
                    f"   Scan completed, but with critical warnings or errors. ScanResult uuid {uuid} for refs {refs} started at {start_time}, ended at {end_time}. Scan logs: {scan_logs}"
                )
            elif status == "STATUS_FAILURE":
                scan_logs = scan_result["spec"].get("logs")
                raise RuntimeError(
                    f"   Scan failed. ScanResult uuid {uuid} for refs {refs} started at {start_time}, ended at {end_time}. Scan logs: {scan_logs}"
                )

    def get_package_versions(self, filter):
        resource_kind = EndorResourceKind.PACKAGE_VERSION.value
        resource_description = (
            f"{resource_kind} with filter '{filter}' in namespace '{self.namespace}'")
        package_versions = self.get_resources(resource_kind, filter=filter)
        self._check_resource(package_versions, resource_description)
        return package_versions

    def export_sbom(
            self,
            package_version_uuid=None,
            package_version_uuids=None,
            package_version_name=None,
            app_name=None,
            project_name=None,
            project_uuid=None,
    ):
        """Export an SBOM from Endor Labs.

        Valid parameter sets (other combinations result in an error from 'endorctl'):
        Single-Package SBOM:
            package_version_uuid
            package_version_name
        Multi-Package SBOM:
            package_version_uuids,app_name
            project_uuid,app_name,app_name
            project_name,app_name,app_name

        https://docs.endorlabs.com/endorctl/commands/sbom/export/
        """
        if package_version_uuids:
            package_version_uuids = ",".join(package_version_uuids)
        return self._call_endorctl(
            "sbom",
            "export",
            package_version_uuid=package_version_uuid,
            package_version_uuids=package_version_uuids,
            package_version_name=package_version_name,
            app_name=app_name,
            project_name=project_name,
            project_uuid=project_uuid,
        )

    # endregion resource functions

    # region workflow functions
    # pylint: disable=unsubscriptable-object
    def get_sbom_for_commit(self, git_url: str, commit_sha: str) -> dict:
        """Export SBOM for the PR commit (sha)."""

        endorfilter = EndorFilter()

        try:
            # Project: get uuid
            project = self.get_project(git_url)
            project_uuid = project["uuid"]
            app_name = project["spec"]["git"]["full_name"]

            # RepositoryVersion: get the context for the PR scan
            endorfilter.context_type = EndorContextType.CI_RUN.value
            filter_str = endorfilter.repository_version(project_uuid, commit_sha)
            repository_version = self.get_repository_version(filter_str)
            assert repository_version is not None
            context_id = repository_version["context"]["id"]

            # ScanResult: wait for a completed scan
            endorfilter.context_id = context_id
            filter_str = endorfilter.scan_result(project_uuid)
            self.get_scan_result(filter_str)

            # PackageVersions: get package versions for SBOM
            filter_str = endorfilter.package_version(project_uuid)
            package_versions = self.get_package_versions(filter_str)
            #
            package_version_uuids = [
                package_version["uuid"]  # type: ignore[reportOptionalSubscript]
                for package_version in package_versions
            ]
            package_version_names = [
                package_version["meta"]["name"]  # type: ignore[reportOptionalSubscript]
                for package_version in package_versions
            ]

            # Export SBOM
            sbom = self.export_sbom(package_version_uuids=package_version_uuids, app_name=app_name)
            print(
                f"Retrieved: CycloneDX SBOM for PackageVersion(s), name: {package_version_names}, uuid: {package_version_uuids}"
            )
            return sbom

        except Exception as ex:
            print(f"Exception: {ex}")
            return {}

    def get_sbom_for_branch(self, git_url: str, branch: str) -> dict:
        """Export lastest SBOM for a monitored branch/ref."""

        endorfilter = EndorFilter()

        try:
            # Project: get uuid
            project = self.get_project(git_url)
            project_uuid = project["uuid"]
            app_name = project["spec"]["git"]["full_name"]

            # RepositoryVersion: get the context for the latest branch scan
            filter_str = endorfilter.repository_version(project_uuid, ref=branch)
            repository_version = self.get_repository_version(filter_str)
            assert repository_version is not None
            repository_version_uuid = repository_version["uuid"]
            repository_version_ref = repository_version["spec"]["version"]["ref"]
            repository_version_sha = repository_version["spec"]["version"]["sha"]
            repository_version_scan_object_status = repository_version["scan_object"]["status"]
            if repository_version_scan_object_status != "STATUS_SCANNED":
                logger.warning(
                    "RepositoryVersion (uuid: %s, ref: %s, sha: %s) scan status is '%s' (expected 'STATUS_SCANNED')",
                    repository_version_uuid, repository_version_ref, repository_version_sha,
                    repository_version_scan_object_status)

            # ScanResult: search for a completed scan
            filter_str = endorfilter.scan_result(EndorContextType.MAIN, project_uuid,
                                                 repository_version_ref, repository_version_sha)
            scan_result = self.get_scan_result(filter_str, retry=False)
            project_uuid = scan_result["meta"]["parent_uuid"]

            # PackageVersions: get package versions for SBOM
            if branch == "master":
                context_type = EndorContextType.MAIN
                context_id = "default"
            else:
                context_type = EndorContextType.REF
                context_id = branch
            filter_str = endorfilter.package_version(context_type, context_id, project_uuid)
            package_version = self.get_package_versions(filter_str)[0]
            assert package_version is not None
            package_version_name = package_version["meta"]["name"]
            package_version_uuid = package_version["uuid"]

            # Export SBOM
            sbom = self.export_sbom(package_version_uuid=package_version_uuid, app_name=app_name)
            logger.info("SBOM: Retrieved CycloneDX SBOM for PackageVersion, name: %s, uuid %s",
                        package_version_name, package_version_uuid)
            return sbom

        except Exception as ex:
            print(f"Exception: {ex}")
            return {}

    def get_sbom_for_project(self, git_url: str) -> dict:
        """Export latest SBOM for EndorCtl project default branch."""

        try:
            # Project: get uuid
            project = self.get_project(git_url)
            project_uuid = project["uuid"]
            app_name = project["spec"]["git"]["full_name"]

            # Export SBOM
            sbom = self.export_sbom(project_uuid=project_uuid, app_name=app_name)
            logger.info("Retrieved: CycloneDX SBOM for Project %s", app_name)
            return sbom

        except Exception as ex:
            print(f"Exception: {ex}")
            return {}

    # endregion workflow functions
