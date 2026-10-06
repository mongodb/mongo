<#
.SYNOPSIS
  Run the SSPI GSSAPI E2E jstest as SYSTEM (the machine account) and stream back its output.

.DESCRIPTION
  mongod's SSPI acceptor gets its inbound credential from the process identity. The SPN is on the
  machine account, whose key is the only one in the LSA (the ssh Administrator session has none, so
  mongod started from it fails with "No credentials are available"). So the shell -- and the mongod
  it spawns -- must run as SYSTEM.

  Windows has no sudo; a one-shot scheduled task is the built-in way to run as SYSTEM. Since a fired
  task returns nothing, it wraps a .cmd that logs output and writes its exit code to a file; this
  script waits on that file, replays the log, and propagates the code.
#>
param(
    [Parameter(Mandatory = $true)][string]$Realm,
    [Parameter(Mandatory = $true)][string]$ServiceName,
    [Parameter(Mandatory = $true)][string]$ServiceHostname,
    [Parameter(Mandatory = $true)][string]$UserPrincipal,
    [Parameter(Mandatory = $true)][string]$UserPassword
)

$ErrorActionPreference = "Stop"

# The payload extracts to <home>\work; this script lives in work\evergreen\gssapi_windows.
$work = (Resolve-Path "$PSScriptRoot\..\..").Path
$bindir = Join-Path $work "dist-test\bin"
$jstest = (Join-Path $work "src\mongo\db\modules\enterprise\jstests\external_auth\gssapi_windows_sspi_e2e.js").Replace('\', '/')
$outLog = Join-Path $work "e2e_output.log"
$exitFile = Join-Path $work "e2e_exit.txt"
$cmdFile = Join-Path $work "run_e2e.cmd"
$taskName = "MongoGssapiE2E"

Remove-Item -Force -ErrorAction SilentlyContinue $outLog, $exitFile

# Single-quoted JS strings inside the double-quoted argument keep the .cmd free of escaping.
$evalArg = "TestData={kerberosRealm:'$Realm',kerberosServiceName:'$ServiceName',kerberosServiceHostname:'$ServiceHostname',kerberosUserPrincipal:'$UserPrincipal',kerberosUserPassword:'$UserPassword'};"

# The shell resolves a bare "mongo"/"mongod" only in the cwd, so cd into the bin dir first.
@"
@echo off
cd /d $bindir
mongo.exe --nodb --eval "$evalArg" "$jstest" > "$outLog" 2>&1
echo %errorlevel% > "$exitFile"
"@ | Set-Content -Path $cmdFile -Encoding ASCII

schtasks /Create /TN $taskName /RU SYSTEM /SC ONCE /ST 00:00 /TR "cmd /c `"$cmdFile`"" /F | Out-Host
schtasks /Run /TN $taskName | Out-Host

# The test starts a mongod and authenticates once over GSSAPI; 20 minutes is a generous margin for
# a freshly promoted DC host.
$deadline = (Get-Date).AddMinutes(20)
while (-not (Test-Path $exitFile)) {
    if ((Get-Date) -gt $deadline) {
        Write-Host "ERROR: E2E jstest did not finish within 20 minutes; partial output:"
        if (Test-Path $outLog) { Get-Content $outLog | Out-Host }
        schtasks /End /TN $taskName 2>$null | Out-Null
        schtasks /Delete /TN $taskName /F 2>$null | Out-Null
        exit 1
    }
    Start-Sleep -Seconds 15
}
schtasks /Delete /TN $taskName /F 2>$null | Out-Null

Write-Host "=== E2E jstest output (run as SYSTEM) ==="
Get-Content $outLog | Out-Host
$code = [int](Get-Content $exitFile | Select-Object -First 1).Trim()
Write-Host "=== E2E jstest exit code: $code ==="
exit $code
