"""Tools for detecting changes inside of Evergreen."""
from typing import List

from git import Repo
from requests import HTTPError
import yaml

from evergreen import EvergreenApi

from buildscripts.patch_builds.change_data import RevisionMap, generate_revision_map


def generate_revision_map_from_manifest(repos: List[Repo], task_id: str,
                                        evg_api: EvergreenApi) -> RevisionMap:
    """
    Generate a revision map for the given repositories using the revisions from the manifest.

    :param repos: Repositories to generate map for.
    :param task_id: Id of evergreen task running.
    :param evg_api: Evergreen API object.
    :return: Map of repositories to revisions
    """
    try:
        manifest = evg_api.manifest_for_task(task_id)
        revisions_data = {
            module_name: module.revision
            for module_name, module in manifest.modules.items()
        }
        revisions_data["mongo"] = manifest.revision
        return generate_revision_map(repos, revisions_data)
    except HTTPError as err:
        # Manifest unavailable from Evergreen? Just make a fake one:
        if err.response.status_code == 404:
            with open("../expansions.yml") as file_handle:
                return generate_revision_map(repos,
                                             {"mongo": yaml.safe_load(file_handle)["revision"]})
        # Some other Evergreen error? Raise it up the stack!
        else:
            raise err
