param (
    [bool]$configure = $false,
    [bool]$build = $false,
    [string]$vcvars_bat = $null
)

function Find-VcVars {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found at $vswhere"
    }

    # 17 is the version of Visual Studio 2022.
    $installPath = & $vswhere `
        -version "[17.0,)" `
        -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath `
        -nologo

    if ($LastExitCode -ne 0 -or -not $installPath) {
        throw "vswhere did not find any suitable Visual Studio installations."
    }

    $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $candidate) {
        return $candidate
    } else {
        throw "vcvars64.bat was not found at expected path: $candidate"
    }
}

if (-not $vcvars_bat) {
    $vcvars_bat = Find-VcVars
}

function Die-On-Failure {
    param (
        $Result
    )

    if ($Result -ne 0) {
        throw("Task failed: " + $Result)
    }
}

# This script should fail when a cmdlet fails.
$ErrorActionPreference = "Stop"

# Source the vcvars to ensure we have access to the Visual Studio toolchain
cmd /c "`"$vcvars_bat`"&set" |
foreach {
    if ($_ -match "=") {
        $v = $_.split("="); set-item -force -path "ENV:\$($v[0])"  -value "$($v[1])"
    }
}

# Ensure the PROCESSOR_ARCHITECTURE environment variable is set. This is sometimes not set
# when entering from a cygwin environment.
$env:PROCESSOR_ARCHITECTURE = "AMD64"

$SWIG_EXECUTABLE = "C:/swigwin-4.2.1/swig.exe"

if (-not (Test-Path cmake_build)) {
    mkdir cmake_build
}

cd cmake_build

# Configure build with CMake.
if ($configure -eq $true) {
    cmake --version
    # Note that ${args} are all the command line options that are not automatically parsed by the param function.
    cmake --no-warn-unused-cli -DSWIG_EXECUTABLE="$SWIG_EXECUTABLE" -DCMAKE_TOOLCHAIN_FILE='..\cmake\toolchains\cl.cmake' ${args} -G "Ninja" ..\.
    Die-On-Failure($LastExitCode)
}

# Execute Ninja build.
if ($build -eq $true) {
    ninja
    Die-On-Failure($LastExitCode)
}
