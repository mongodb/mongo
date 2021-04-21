"""Tools for detecting changes inside of Evergreen."""
from typing import List

from git import Repo
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
    manifest = evg_api.manifest_for_task(task_id)
    revisions_data = {
        module_name: module.revision
        for module_name, module in manifest.modules.items()
    }
    revisions_data["mongo"] = manifest.revision

    return generate_revision_map(repos, revisions_data)
