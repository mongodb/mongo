#!/bin/env bash
set -eux

# get the datetime for the current commit SHA
cd ${WORK_DIR}/src
commit_datetime=$(git show -s --format=%cd --date=iso-strict ${GITHUB_COMMIT})
echo "Date and time of commit: $commit_datetime"

# generate the SAST report
cd ${MODULE_PATH}/scripts
echo "Running SAST report generation script..."
virtualenv -p python3.12 .venv
source .venv/bin/activate
pip install -r sast_reporting/requirements.txt
if [ -z "${TRIGGERED_BY_GIT_TAG}" ]; then
    echo "Evergreen version was NOT triggered by a git tag"
    echo "Setting Google Drive folder ID for non-release"
    google_drive_folder_id="${SAST_REPORT_TEST_GOOGLE_DRIVE_FOLDER_ID}"
else
    echo "Evergreen version was triggered by git tag '${TRIGGERED_BY_GIT_TAG}'"
    echo "Setting Google Drive folder ID for release"
    google_drive_folder_id="${SAST_REPORT_RELEASES_GOOGLE_DRIVE_FOLDER_ID}"
fi
python3 -m sast_reporting.src.mongodb_server \
    --version ${MONGODB_VERSION} \
    --branch ${MONGODB_RELEASE_BRANCH} \
    --commit-date $commit_datetime \
    --output-path ${MODULE_PATH}/sast_report_${MONGODB_VERSION}.xlsx \
    --upload-file-name "[${MONGODB_VERSION}] MongoDB Server Enterprise SAST Report" \
    --google-drive-folder-id $google_drive_folder_id \
    --env-file ${WORK_DIR}/sast_report_generation_credentials.env
