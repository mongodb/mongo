param (
    [bool]$configure = $false,
    [bool]$build = $false,
    [string]$vcvars_bat = "C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars64.bat"
)

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

# Ensure the swig binary location is in our PATH.
$env:Path += ";C:\swigwin-3.0.2"

if (-not (Test-Path cmake_build)) {
    mkdir cmake_build
}

cd cmake_build

# Configure build with CMake.
if ($configure -eq $true) {
    # Note that ${args} are all the command line options that are not automatically parsed by the param function.
    C:\cmake\bin\cmake --no-warn-unused-cli -DSWIG_DIR='C:\swigwin-3.0.2' -DSWIG_EXECUTABLE='C:\swigwin-3.0.2\swig.exe' -DCMAKE_TOOLCHAIN_FILE='..\cmake\toolchains\cl.cmake' ${args} -G "Ninja" ..\.
    Die-On-Failure($LastExitCode)
}

# Execute Ninja build.
if ($build -eq $true) {
    ninja
    Die-On-Failure($LastExitCode)
}
