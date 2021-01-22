"""Powercycle constants."""

EXPANSIONS_FILE = "expansions.yml"
DEFAULT_SSH_CONNECTION_OPTIONS = ("-o ServerAliveCountMax=10"
                                  " -o ServerAliveInterval=6"
                                  " -o StrictHostKeyChecking=no"
                                  " -o ConnectTimeout=30"
                                  " -o ConnectionAttempts=20")

MONITOR_PROC_FILE = "proc.json"
MONITOR_SYSTEM_FILE = "system.json"
EC2_MONITOR_FILES = f"{MONITOR_PROC_FILE} {MONITOR_SYSTEM_FILE}"
STANDARD_PORT = 20000
SECRET_PORT = 20001
REMOTE_DIR = "/log/powercycle"
VIRTUALENV_DIR = "venv_powercycle"
RESMOKE_PATH = "buildscripts/resmoke.py"
WINDOWS_CRASH_DL = "https://download.sysinternals.com/files/NotMyFault.zip"
WINDOWS_CRASH_DIR = "notmyfault"
WINDOWS_CRASH_ZIP = "notmyfault.zip"
WINDOWS_CRASH_CMD = "notmyfault/notmyfaultc64.exe -accepteula crash 1"

BACKUP_PATH_AFTER = f"{REMOTE_DIR}/afterrecovery"
BACKUP_PATH_BEFORE = f"{REMOTE_DIR}/beforerecovery"
BACKUP_ARTIFACTS = f"{BACKUP_PATH_AFTER}* {BACKUP_PATH_BEFORE}*"
DB_PATH = "/data/db"
LOG_PATH = f"{REMOTE_DIR}/mongod.log"
EVENT_LOGPATH = f"{REMOTE_DIR}/eventlog"
EC2_ARTIFACTS = f"{LOG_PATH} {DB_PATH} {BACKUP_ARTIFACTS} {EVENT_LOGPATH}"

DB_NAME = "power"
COLLECTION_NAME = "cycle"
RSYNC_EXCLUDE_FILES = ["diagnostic.data/metrics.interim*"]

CRASH_WAIT_TIME = 45
CRASH_WAIT_TIME_JITTER = 5

NUM_CRUD_CLIENTS = 20
CRUD_CLIENT = "jstests/hooks/crud_client.js"
CONFIG_CRUD_CLIENT = "buildscripts/resmokeconfig/suites/with_external_server.yml"
NUM_FSM_CLIENTS = 20
FSM_CLIENT = "jstests/libs/fsm_serial_client.js"
SET_READ_AND_WRITE_CONCERN = "jstests/libs/override_methods/set_read_and_write_concerns.js"

REPORT_JSON_FILE = "report.json"
POWERCYCLE_EXIT_FILE = "powercycle_exit.yml"

DEFAULT_CRASH_METHOD = "internal"
DEFAULT_TEST_LOOPS = 15
DEFAULT_SEED_DOC_NUM = 10_000
DEFAULT_MONGOD_OPTIONS = ("--setParameter enableTestCommands=1"
                          " --setParameter logComponentVerbosity='{storage:{recovery:2}}'"
                          " --storageEngine wiredTiger"
                          " --wiredTigerEngineConfigString 'debug_mode=[table_logging=true]'")
