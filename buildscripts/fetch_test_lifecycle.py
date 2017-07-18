#!/usr/bin/env python

"""Script to retrieve the etc/test_lifecycle.yml tag file from the metadata repository that
corresponds to the current repository.

Usage:
    python buildscsripts/fetch_test_lifecycle.py evergreen-project revision
"""

from __future__ import absolute_import
from __future__ import print_function

import logging
import optparse
import os
import posixpath
import shutil
import sys
import textwrap

import yaml

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts import git


LOGGER = logging.getLogger(__name__)


class MetadataRepository(object):
    """Represent the metadata repository containing the test lifecycle tags file."""

    def __init__(self, repository, references_file, lifecycle_file):
        """Initlialize the MetadataRepository.

        Args:
            repository: the git.Repository object for the repository.
            references_file: the relative path from the root of the repository to the references
                yaml file.
            lifecycle_file: the relative path from the root of the repository to the test lifecycle
                tags yaml file.
        """
        self._repository = repository
        self._references_file = references_file
        self._lifecycle_file = lifecycle_file
        # The path to the lifecycle file, absolute or relative to the current working directory.
        self.lifecycle_path = os.path.join(repository.directory, lifecycle_file)

    def list_revisions(self):
        """List the revisions from the HEAD of this repository.

        Returns:
            A list of str containing the git hashes for all the revisions from the newest (HEAD)
            to the oldest.
            """
        return self._repository.git_rev_list(["HEAD", "--", self._lifecycle_file]).splitlines()

    def _get_references_content(self, revision):
        references_content = self._repository.git_cat_file(
            ["blob", "%s:%s" % (revision, self._references_file)])
        return references_content

    def get_reference(self, metadata_revision, project):
        """Retrieve the reference revision (a revision of the project 'project') associated with
         the test lifecycle file present in the metadata repository at revision 'metadata_revision'.

         Args:
             metadata_revision: a revision (git hash) of this repository.
             project: an Evergreen project name (e.g. mongodb-mongo-master).
         """
        references_content = self._get_references_content(metadata_revision)
        references = yaml.safe_load(references_content)
        return references.get("test-lifecycle", {}).get(project)

    def get_lifecycle_file_content(self, metadata_revision):
        """Return the content of the test lifecycle file as it was at the given revision."""
        return self._repository.git_cat_file(["blob", "%s:%s" % (metadata_revision,
                                                                 self._lifecycle_file)])


def _clone_repository(url, branch):
    """Clone the repository present at the URL 'url' and use the branch 'branch'."""
    target_directory = posixpath.splitext(posixpath.basename(url))[0]
    LOGGER.info("Cloning the repository %s into the directory %s", url, target_directory)
    return git.Repository.clone(url, target_directory, branch)


def _get_metadata_revision(metadata_repo, mongo_repo, project, revision):
    """Get the metadata revision that corresponds to a given repository, project, revision."""
    for metadata_revision in metadata_repo.list_revisions():
        reference = metadata_repo.get_reference(metadata_revision, project)
        if not reference:
            # No reference for this revision. This should not happen but we keep trying in
            # case we can find an older revision with a reference.
            continue
        if mongo_repo.is_ancestor(reference, revision):
            # We found a reference that is a parent of the current revision.
            return metadata_revision
    return None


def fetch_test_lifecycle(metadata_repo_url, references_file, lifecycle_file, project, revision):
    """Fetch the test lifecycle file that corresponds to the given revision of the repository this
    script is called from.

    Args:
        metadata_repo_url: the git repository URL for the metadata repository containing the test
            lifecycle file.
        references_file: the relative path from the root of the metadata repository to the
            references file.
        lifecycle_file: the relative path from the root of the metadata repository to the test
            lifecycle file.
        project: the Evergreen project name.
        revision: the current repository revision.
    """
    metadata_repo = MetadataRepository(_clone_repository(metadata_repo_url, project),
                                       references_file, lifecycle_file)
    mongo_repo = git.Repository(os.getcwd())
    metadata_revision = _get_metadata_revision(metadata_repo, mongo_repo, project, revision)
    if metadata_revision:
        LOGGER.info("Using metadata repository revision '%s'", metadata_revision)
        result = metadata_repo.get_lifecycle_file_content(metadata_revision)
    else:
        result = None
    return result


def main():
    """
    Utility to fetch the etc/test_lifecycle.yml file corresponding to a given revision from
    the mongo-test-metadata repository.
    """
    parser = optparse.OptionParser(description=textwrap.dedent(main.__doc__),
                                   usage="Usage: %prog [options] evergreen-project")

    parser.add_option("--revision", dest="revision",
                      metavar="<revision>",
                      default="HEAD",
                      help=("The project revision for which to retrieve the test lifecycle tags"
                            " file."))

    parser.add_option("--metadataRepo", dest="metadata_repo_url",
                      metavar="<metadata-repo-url>",
                      default="git@github.com:mongodb/mongo-test-metadata.git",
                      help=("The URL to the metadata repository that contains the test lifecycle"
                            " tags file."))

    parser.add_option("--lifecycleFile", dest="lifecycle_file",
                      metavar="<lifecycle-file>",
                      default="etc/test_lifecycle.yml",
                      help=("The path to the test lifecycle tags file, relative to the root of the"
                            " metadata repository. Defaults to '%default'."))

    parser.add_option("--referencesFile", dest="references_file",
                      metavar="<references-file>",
                      default="references.yml",
                      help=("The path to the metadata references file, relative to the root of the"
                            " metadata repository. Defaults to '%default'."))

    parser.add_option("--destinationFile", dest="destination_file",
                      metavar="<destination-file>",
                      default="etc/test_lifecycle.yml",
                      help=("The path where the lifecycle file should be available when this script"
                            " completes successfully. This path is absolute or relative to the"
                            " current working directory. Defaults to '%default'."))

    parser.add_option("--logLevel", dest="log_level",
                      metavar="<log-level>",
                      choices=["DEBUG", "INFO", "WARNING", "ERROR"],
                      default="INFO",
                      help="The log level: DEBUG, INFO, WARNING or ERROR. Defaults to '%default'.")

    parser.add_option("--logFile", dest="log_file",
                      metavar="<log-file>",
                      default=None,
                      help=("The destination file for the logs. If not set the script will log to"
                            " the standard output"))

    options, args = parser.parse_args()

    if len(args) != 1:
        parser.print_help(file=sys.stderr)
        print(file=sys.stderr)
        parser.error("Must specify an Evergreen project")
    evergreen_project = args[0]

    logging.basicConfig(format="%(asctime)s %(levelname)s %(message)s",
                        level=options.log_level, filename=options.log_file)

    lifecycle_file_content = fetch_test_lifecycle(options.metadata_repo_url,
                                                  options.references_file,
                                                  options.lifecycle_file,
                                                  evergreen_project,
                                                  options.revision)
    if not lifecycle_file_content:
        LOGGER.error("Failed to fetch the test lifecycle tag file.")
        sys.exit(1)
    else:
        LOGGER.info("Writing the test lifecycle file to '%s'.", options.destination_file)
        with open(options.destination_file, "wb") as destf:
            destf.write(lifecycle_file_content)
        LOGGER.info("Done.")


if __name__ == "__main__":
    main()
