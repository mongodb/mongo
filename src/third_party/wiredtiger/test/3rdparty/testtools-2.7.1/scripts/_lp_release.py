#!/usr/bin/python

"""Release testtools on Launchpad.

Steps:
 1. Make sure all "Fix committed" bugs are assigned to 'next'
 2. Rename 'next' to the new version
 3. Release the milestone
 4. Upload the tarball
 5. Create a new 'next' milestone
 6. Mark all "Fix committed" bugs in the milestone as "Fix released"

Assumes that NEWS is in the parent directory, that the release sections are
underlined with '~' and the subsections are underlined with '-'.

Assumes that this file is in the 'scripts' directory a testtools tree that has
already had a tarball built and uploaded with 'python setup.py sdist upload
--sign'.
"""

from datetime import datetime, timedelta, tzinfo
import logging
import os
import sys

from launchpadlib.launchpad import Launchpad
from launchpadlib import uris


APP_NAME = 'testtools-lp-release'
CACHE_DIR = os.path.expanduser('~/.launchpadlib/cache')
SERVICE_ROOT = uris.LPNET_SERVICE_ROOT

FIX_COMMITTED = "Fix Committed"
FIX_RELEASED = "Fix Released"

# Launchpad file type for a tarball upload.
CODE_RELEASE_TARBALL = 'Code Release Tarball'

PROJECT_NAME = 'testtools'
NEXT_MILESTONE_NAME = 'next'


class _UTC(tzinfo):
    """UTC"""

    def utcoffset(self, dt):
        return timedelta(0)

    def tzname(self, dt):
        return "UTC"

    def dst(self, dt):
        return timedelta(0)

UTC = _UTC()


def configure_logging():
    level = logging.INFO
    log = logging.getLogger(APP_NAME)
    log.setLevel(level)
    handler = logging.StreamHandler()
    handler.setLevel(level)
    formatter = logging.Formatter("%(levelname)s: %(message)s")
    handler.setFormatter(formatter)
    log.addHandler(handler)
    return log
LOG = configure_logging()


def get_path(relpath):
    """Get the absolute path for something relative to this file."""
    return os.path.abspath(
        os.path.join(
            os.path.dirname(os.path.dirname(__file__)), relpath))


def assign_fix_committed_to_next(testtools, next_milestone):
    """Find all 'Fix Committed' and make sure they are in 'next'."""
    fixed_bugs = list(testtools.searchTasks(status=FIX_COMMITTED))
    for task in fixed_bugs:
        LOG.debug(f"{task.title}")
        if task.milestone != next_milestone:
            task.milestone = next_milestone
            LOG.info(f"Re-assigning {task.title}")
            task.lp_save()


def rename_milestone(next_milestone, new_name):
    """Rename 'next_milestone' to 'new_name'."""
    LOG.info(f"Renaming {next_milestone.name} to {new_name}")
    next_milestone.name = new_name
    next_milestone.lp_save()


def get_release_notes_and_changelog(news_path):
    release_notes = []
    changelog = []
    state = None
    last_line = None

    def is_heading_marker(line, marker_char):
        return line and line == marker_char * len(line)

    LOG.debug(f"Loading NEWS from {news_path}")
    with open(news_path) as news:
        for line in news:
            line = line.strip()
            if state is None:
                if (is_heading_marker(line, '~') and
                    not last_line.startswith('NEXT')):
                    milestone_name = last_line
                    state = 'release-notes'
                else:
                    last_line = line
            elif state == 'title':
                # The line after the title is a heading marker line, so we
                # ignore it and change state. That which follows are the
                # release notes.
                state = 'release-notes'
            elif state == 'release-notes':
                if is_heading_marker(line, '-'):
                    state = 'changelog'
                    # Last line in the release notes is actually the first
                    # line of the changelog.
                    changelog = [release_notes.pop(), line]
                else:
                    release_notes.append(line)
            elif state == 'changelog':
                if is_heading_marker(line, '~'):
                    # Last line in changelog is actually the first line of the
                    # next section.
                    changelog.pop()
                    break
                else:
                    changelog.append(line)
            else:
                raise ValueError("Couldn't parse NEWS")

    release_notes = '\n'.join(release_notes).strip() + '\n'
    changelog = '\n'.join(changelog).strip() + '\n'
    return milestone_name, release_notes, changelog


def release_milestone(milestone, release_notes, changelog):
    date_released = datetime.now(tz=UTC)
    LOG.info(
        f"Releasing milestone: {milestone.name}, date {date_released}")
    release = milestone.createProductRelease(
        date_released=date_released,
        changelog=changelog,
        release_notes=release_notes,
        )
    milestone.is_active = False
    milestone.lp_save()
    return release


def create_milestone(series, name):
    """Create a new milestone in the same series as 'release_milestone'."""
    LOG.info(f"Creating milestone {name} in series {series.name}")
    return series.newMilestone(name=name)


def close_fixed_bugs(milestone):
    tasks = list(milestone.searchTasks())
    for task in tasks:
        LOG.debug(f"Found {task.title}")
        if task.status == FIX_COMMITTED:
            LOG.info(f"Closing {task.title}")
            task.status = FIX_RELEASED
        else:
            LOG.warning(
                f"Bug not fixed, removing from milestone: {task.title}")
            task.milestone = None
        task.lp_save()


def upload_tarball(release, tarball_path):
    with open(tarball_path) as tarball:
        tarball_content = tarball.read()
    sig_path = tarball_path + '.asc'
    with open(sig_path) as sig:
        sig_content = sig.read()
    tarball_name = os.path.basename(tarball_path)
    LOG.info(f"Uploading tarball: {tarball_path}")
    release.add_file(
        file_type=CODE_RELEASE_TARBALL,
        file_content=tarball_content, filename=tarball_name,
        signature_content=sig_content,
        signature_filename=sig_path,
        content_type="application/x-gzip; charset=binary")


def release_project(launchpad, project_name, next_milestone_name):
    testtools = launchpad.projects[project_name]
    next_milestone = testtools.getMilestone(name=next_milestone_name)
    release_name, release_notes, changelog = get_release_notes_and_changelog(
        get_path('NEWS'))
    LOG.info(f"Releasing {project_name} {release_name}")
    # Since reversing these operations is hard, and inspecting errors from
    # Launchpad is also difficult, do some looking before leaping.
    errors = []
    tarball_path = get_path(f'dist/{project_name}-{release_name}.tar.gz')
    if not os.path.isfile(tarball_path):
        errors.append(f"{tarball_path} does not exist")
    if not os.path.isfile(tarball_path + '.asc'):
        errors.append("{} does not exist".format(tarball_path + '.asc'))
    if testtools.getMilestone(name=release_name):
        errors.append(f"Milestone {release_name} exists on {project_name}")
    if errors:
        for error in errors:
            LOG.error(error)
        return 1
    assign_fix_committed_to_next(testtools, next_milestone)
    rename_milestone(next_milestone, release_name)
    release = release_milestone(next_milestone, release_notes, changelog)
    upload_tarball(release, tarball_path)
    create_milestone(next_milestone.series_target, next_milestone_name)
    close_fixed_bugs(next_milestone)
    return 0


def main(args):
    launchpad = Launchpad.login_with(
        APP_NAME, SERVICE_ROOT, CACHE_DIR, credentials_file='.lp_creds')
    return release_project(launchpad, PROJECT_NAME, NEXT_MILESTONE_NAME)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
