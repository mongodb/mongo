# The Windows counterpart of test_wrapper.sh: the --run_under for bazel test on Windows.
#
# Usage (through --run_under=//bazel:windows_test_launcher): wtl.exe TEST_BIN [ARGS...]
#
# Bazel's Windows test runner starts --run_under programs with CreateProcessW, so it cannot run
# test_wrapper.sh; wtl.exe starts this script with a NUL-delimited argument payload. It ports the parts of
# test_wrapper.sh that apply to Windows:
#
# * It runs the test binary from its original bazel-bin location instead of its runfiles copy.
#   Bazel starts Windows tests inside their runfiles tree, where the test path points at
#   <name>.exe.runfiles/_main/<package>/<name>.exe. That doubles the package path, and tests in
#   deep packages end up past the ~256-character limit of dbghelp's SymInitialize, which gtest's
#   abseil symbolizer treats as fatal. The original binary is the runfiles directory without its
#   ".runfiles" suffix, and its path is at most ~190 characters. The working directory and the
#   runfiles environment are left unchanged, so tests still find their data through runfiles.
# * It unsets the golden test variables, which are incompatible with remote execution.
# * Before Bazel's test timeout, it writes a minidump of a test that is still running and then
#   stops it, which is what test_wrapper.sh does with SIGABRT and core files on Linux. The dump is
#   written to TEST_UNDECLARED_OUTPUTS_DIR so Bazel retains it and Evergreen collects it. Its
#   dump_<binary>.<pid>.mdmp name lets the Windows core analyzer find the executable and symbols.
#

$ErrorActionPreference = 'Stop'

if ($args.Count -ne 1) {
    [Console]::Error.WriteLine('windows_test_launcher: expected the native launcher argument payload')
    exit 2
}

$payloadPath = [string]$args[0]
try {
    $payload = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($payloadPath))
} finally {
    # Read before starting the test: a test or host termination must not leave the payload behind.
    Remove-Item -LiteralPath $payloadPath -ErrorAction SilentlyContinue
}
if (-not $payload.EndsWith([string][char]0)) {
    throw 'windows_test_launcher: malformed argument payload'
}
$launchArgs = $payload.Substring(0, $payload.Length - 1).Split([char]0)
$testBin = $launchArgs[0]
$testArgs = @($launchArgs | Select-Object -Skip 1)

# Golden tests are currently incompatible with Bazel Remote Execution.
Remove-Item Env:GOLDEN_TEST_CONFIG_PATH -ErrorAction SilentlyContinue
Remove-Item Env:GOLDEN_TEST_OUTPUT_ROOT_PATTERN -ErrorAction SilentlyContinue

$runfilesDir = $env:RUNFILES_DIR
if (-not $runfilesDir) {
    $runfilesDir = $env:TEST_SRCDIR
}
if ($runfilesDir) {
    $runfilesDir = $runfilesDir.TrimEnd('/', '\')
    if ($runfilesDir.EndsWith('.runfiles', [StringComparison]::OrdinalIgnoreCase)) {
        $originalBin = $runfilesDir.Substring(0, $runfilesDir.Length - '.runfiles'.Length)
        $sameName = [IO.Path]::GetFileName($originalBin) -ieq [IO.Path]::GetFileName($testBin.Replace('/', '\'))
        if ($sameName -and (Test-Path -LiteralPath $originalBin -PathType Leaf)) {
            # RUNFILES_DIR uses forward slashes. Hand Windows a native path: some programs
            # (cmd.exe among them) parse "/..." segments of their own path as switches.
            $testBin = $originalBin.Replace('/', '\')
        }
    }
}

# Prints a launcher message to stderr, which Bazel merges into the test log. Windows PowerShell 5.1
# holds stdout written to a pipe (as Bazel's test runner captures it) until the process exits,
# even through Write-Output or [Console]::Out.Flush(), and the runner drops what arrives that
# late; stderr is written immediately.
function Write-LauncherMessage([string]$message) {
    [Console]::Error.WriteLine($message)
}

# Quotes one argument so CommandLineToArgvW and the MSVC CRT parse it back unchanged.
function ConvertTo-WindowsArgument([string]$arg) {
    if ($arg.Length -gt 0 -and $arg -notmatch '[\s"]') {
        return $arg
    }
    $quoted = New-Object System.Text.StringBuilder
    [void]$quoted.Append('"')
    $backslashes = 0
    foreach ($char in $arg.ToCharArray()) {
        if ($char -eq '\') {
            $backslashes++
            continue
        }
        if ($char -eq '"') {
            [void]$quoted.Append('\', 2 * $backslashes + 1)
        } elseif ($backslashes -gt 0) {
            [void]$quoted.Append('\', $backslashes)
        }
        $backslashes = 0
        [void]$quoted.Append($char)
    }
    [void]$quoted.Append('\', 2 * $backslashes)
    [void]$quoted.Append('"')
    return $quoted.ToString()
}

# Writes a full-memory minidump of $process to the test's undeclared outputs. The P/Invoke type is only
# compiled when a dump is needed, so passing tests do not pay for it.
function Write-TestMinidump([System.Diagnostics.Process]$process, [string]$binaryPath) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class MongoTestMinidump {
    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool MiniDumpWriteDump(IntPtr process, uint processId,
        SafeFileHandle file, uint dumpType, IntPtr exception, IntPtr userStream, IntPtr callback);
}
'@
    # MiniDumpWithFullMemory | MiniDumpIgnoreInaccessibleMemory, the flags of the full dumps mongo's
    # crash handler writes (src/mongo/util/exception_filter_win32.cpp).
    $dumpType = 0x2 -bor 0x20000
    $dumpDirectory = $env:TEST_UNDECLARED_OUTPUTS_DIR
    if (-not $dumpDirectory) {
        # Keep direct launcher invocations usable outside Bazel's test environment.
        $dumpDirectory = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($binaryPath))
    }
    [void][IO.Directory]::CreateDirectory($dumpDirectory)
    $binaryName = [IO.Path]::GetFileNameWithoutExtension($binaryPath)
    $dumpPath = [IO.Path]::Combine($dumpDirectory, "dump_$binaryName.$($process.Id).mdmp")
    $file = [IO.File]::Create($dumpPath)
    try {
        if ([MongoTestMinidump]::MiniDumpWriteDump($process.Handle, [uint32]$process.Id,
                $file.SafeFileHandle, $dumpType, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)) {
            Write-LauncherMessage "Writing minidump to $dumpPath..."
        } else {
            $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            Write-LauncherMessage "Failed to write a minidump of the test process: Win32 error $err"
        }
    } finally {
        $file.Dispose()
    }
}

# Windows PowerShell 5.1 drops empty arguments and mangles embedded quotes when calling native
# programs with `& $exe @args`, so build the command line explicitly. With UseShellExecute off
# and no redirection, the test inherits this console's stdin, stdout, and stderr.
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $testBin
$startInfo.Arguments = ($testArgs | ForEach-Object { ConvertTo-WindowsArgument $_ }) -join ' '
$startInfo.UseShellExecute = $false
$startInfo.WorkingDirectory = (Get-Location).ProviderPath

$process = [System.Diagnostics.Process]::Start($startInfo)
Write-LauncherMessage "Process-under-test started with PID: $($process.Id)"

# Leave a minute before Bazel's own timeout (TEST_TIMEOUT, in seconds) for the dump. The unit test
# tasks set --test_timeout=660, which gives the 600 seconds test_wrapper.sh uses.
$timeoutSeconds = 0
if ([int]::TryParse([string]$env:TEST_TIMEOUT, [ref]$timeoutSeconds) -and $timeoutSeconds -gt 0) {
    $dumpAfterSeconds = [Math]::Max($timeoutSeconds - 60, [int]($timeoutSeconds * 0.9))
    if (-not $process.WaitForExit($dumpAfterSeconds * 1000)) {
        Write-LauncherMessage "$dumpAfterSeconds second timer finished. Process-under-test $($process.Id) is still running. Writing a minidump and stopping it."
        try {
            Write-TestMinidump $process $testBin
        } finally {
            # Stop the whole process tree, as test_wrapper.sh signals the whole process group: a
            # child left running keeps the test's output pipe open, and Bazel's test runner then
            # drops output that had not reached the test log yet.
            # Bazel sets PATHEXT to .CPL. PowerShell treats even an absolute .exe path
            # as a document, so start taskkill directly without its native invocation.
            $killInfo = New-Object System.Diagnostics.ProcessStartInfo
            $killInfo.FileName = "$env:SystemRoot\System32\taskkill.exe"
            $killInfo.Arguments = "/T /F /PID $($process.Id)"
            $killInfo.UseShellExecute = $false
            $killer = [System.Diagnostics.Process]::Start($killInfo)
            $killer.WaitForExit()
        }
    }
}
$process.WaitForExit()
exit $process.ExitCode
