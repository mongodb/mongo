<# build_rapidyaml_windows_x64.ps1
   Builds a rapidyaml wheel for x86_64-pc-windows-msvc.
   Output: .\dist\rapidyaml-*.whl
#>

$ErrorActionPreference = "Stop"

# ---- Config (override via env before running) -------------------------------
$RAPIDYAML_REPO = if ($env:RAPIDYAML_REPO) { $env:RAPIDYAML_REPO } else { "https://github.com/mongodb-forks/rapidyaml.git" }
$RAPIDYAML_REF = if ($env:RAPIDYAML_REF) { $env:RAPIDYAML_REF } else { "a5d485fd44719e1c03e059177fc1f695fc462b66" }
$RAPIDYAML_VERSION = if ($env:RAPIDYAML_VERSION) { $env:RAPIDYAML_VERSION } else { "" }
$OUT_DIR = if ($env:OUT_DIR) { $env:OUT_DIR } else { Join-Path (Get-Location) "dist" }
$PYTHON_BIN = if ($env:PYTHON_BIN) { $env:PYTHON_BIN } else { "python" }

function Import-MsvcEnvironment {
  if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    return $true
  }

  $CandidateBatches = @()
  if ($env:VCVARS64_BAT) {
    $CandidateBatches += @{
      Path = $env:VCVARS64_BAT
      Args = ""
    }
  }
  if ($env:VSDEVCMD_BAT) {
    $CandidateBatches += @{
      Path = $env:VSDEVCMD_BAT
      Args = "-arch=x64 -host_arch=x64"
    }
  }

  $VswhereCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
    (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
  )

  foreach ($Vswhere in $VswhereCandidates) {
    if (-not $Vswhere -or -not (Test-Path $Vswhere)) {
      continue
    }

    $InstallPath = & $Vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($InstallPath)) {
      continue
    }

    $CandidateBatches += @{
      Path = (Join-Path $InstallPath "VC\Auxiliary\Build\vcvars64.bat")
      Args = ""
    }
    $CandidateBatches += @{
      Path = (Join-Path $InstallPath "Common7\Tools\VsDevCmd.bat")
      Args = "-arch=x64 -host_arch=x64"
    }
  }

  foreach ($Edition in @("Enterprise", "Professional", "Community", "BuildTools")) {
    foreach ($Year in @("2022", "2019")) {
      $Root = Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\$Year\$Edition"
      $CandidateBatches += @{
        Path = (Join-Path $Root "VC\Auxiliary\Build\vcvars64.bat")
        Args = ""
      }
      $CandidateBatches += @{
        Path = (Join-Path $Root "Common7\Tools\VsDevCmd.bat")
        Args = "-arch=x64 -host_arch=x64"
      }
    }
  }

  foreach ($Candidate in $CandidateBatches) {
    if (-not $Candidate.Path -or -not (Test-Path $Candidate.Path)) {
      continue
    }

    Write-Host "==> Importing MSVC environment from $($Candidate.Path)"
    $Command = "call `"$($Candidate.Path)`""
    if ($Candidate.Args) {
      $Command += " $($Candidate.Args)"
    }
    $Command += " >nul && set"

    $EnvLines = & cmd.exe /d /s /c $Command
    if ($LASTEXITCODE -ne 0) {
      continue
    }

    foreach ($Line in $EnvLines) {
      if ($Line -match "^([^=]+)=(.*)$") {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
      }
    }

    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
      return $true
    }
  }

  return $false
}

if ([string]::IsNullOrWhiteSpace($RAPIDYAML_VERSION)) {
  throw "RAPIDYAML_VERSION must be set (for example: 0.9.0.post0)."
}

New-Item -ItemType Directory -Force -Path $OUT_DIR | Out-Null

Write-Host "==> rapidyaml Windows x86_64"
Write-Host "    RAPIDYAML_REF=$RAPIDYAML_REF"
Write-Host "    RAPIDYAML_VERSION=$RAPIDYAML_VERSION"
Write-Host "    OUT_DIR=$OUT_DIR"
Write-Host "    PYTHON_BIN=$PYTHON_BIN"

# ---- Tooling sanity ---------------------------------------------------------
if (-not (Import-MsvcEnvironment)) {
  throw "MSVC (cl.exe) not found in PATH. Run this in 'Developer PowerShell for VS' or install VS Build Tools."
}
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
  throw "git.exe not found in PATH."
}

$BuildRootBase = if ($env:RAPIDYAML_BUILD_ROOT) { $env:RAPIDYAML_BUILD_ROOT } else { Join-Path $env:SystemDrive "tmp" }
New-Item -ItemType Directory -Force -Path $BuildRootBase | Out-Null

$BuildRoot = Join-Path $BuildRootBase ("rapidyaml-build-" + [System.Guid]::NewGuid().ToString("N"))
$RepoDir = Join-Path $BuildRoot "rapidyaml"
$VenvDir = Join-Path $BuildRoot "venv"
$WheelhouseDir = Join-Path $BuildRoot "wheelhouse"
$RepairedDir = Join-Path $BuildRoot "repaired"
$PushedLocation = $false
$OriginalPath = $env:Path

New-Item -ItemType Directory -Force -Path $BuildRoot, $WheelhouseDir, $RepairedDir | Out-Null

try {
  & $PYTHON_BIN -m venv $VenvDir
  $EnvPython = Join-Path $VenvDir "Scripts\python.exe"
  if (-not (Test-Path $EnvPython)) {
    throw "Failed to create virtualenv at $VenvDir."
  }

  git clone $RAPIDYAML_REPO $RepoDir | Out-Null

  Push-Location $RepoDir
  $PushedLocation = $true

  git -c advice.detachedHead=false checkout $RAPIDYAML_REF | Out-Null
  git submodule update --init --recursive | Out-Null

  $env:SETUPTOOLS_SCM_PRETEND_VERSION = $RAPIDYAML_VERSION
  if (-not $env:CMAKE_BUILD_PARALLEL_LEVEL) {
    $env:CMAKE_BUILD_PARALLEL_LEVEL = $env:NUMBER_OF_PROCESSORS
  }
  $env:CMAKE_GENERATOR = "Ninja"

  & $EnvPython -m pip install --upgrade "pip<26" setuptools wheel build delvewheel "packaging<26"
  $env:Path = "$(Join-Path $VenvDir 'Scripts');$env:Path"

  $BuildRequirementsJson = @'
import json
import pathlib
import tomllib


def normalize_requirement(req_string: str) -> str:
    requirement, sep, marker = req_string.partition(";")
    requirement = requirement.strip()

    if "~=" in requirement:
        name, version = requirement.split("~=", 1)
        requirement = f"{name.strip()}=={version.strip()}"

    if sep:
        return f"{requirement}; {marker.strip()}"
    return requirement


data = tomllib.loads(pathlib.Path("pyproject.toml").read_text(encoding="utf-8"))
print(json.dumps([normalize_requirement(req) for req in data["build-system"]["requires"]]))
'@ | & $EnvPython -
  $BuildRequirements = $BuildRequirementsJson | ConvertFrom-Json
  if (-not $BuildRequirements) {
    throw "Unable to read build-system.requires from pyproject.toml."
  }

  foreach ($Requirement in $BuildRequirements) {
    Write-Host "==> Installing build dependency $Requirement"
    & $EnvPython -m pip install --upgrade $Requirement
  }

  $SwigExecutable = Get-Command swig.exe -ErrorAction SilentlyContinue
  if (-not $SwigExecutable) {
    throw "swig.exe not found after installing build dependencies."
  }
  $env:SWIG_EXECUTABLE = $SwigExecutable.Source
  $env:SWIG_DIR = (& $env:SWIG_EXECUTABLE -swiglib).Trim()
  if (-not $env:SWIG_DIR) {
    throw "Failed to resolve SWIG_DIR from $($env:SWIG_EXECUTABLE)."
  }

  & $EnvPython -m build --wheel --no-isolation --outdir $WheelhouseDir

  $Wheel = Get-ChildItem -Path $WheelhouseDir -Filter *.whl | Select-Object -First 1
  if (-not $Wheel) {
    throw "No wheel produced in $WheelhouseDir."
  }

  Write-Host "==> Built wheel: $($Wheel.Name)"
  & $EnvPython -m delvewheel show $Wheel.FullName
  & $EnvPython -m delvewheel repair --wheel-dir $RepairedDir $Wheel.FullName

  $RepairedWheel = Get-ChildItem -Path $RepairedDir -Filter *.whl | Select-Object -First 1
  if (-not $RepairedWheel) {
    throw "No repaired wheel produced in $RepairedDir."
  }

  Copy-Item $RepairedWheel.FullName $OUT_DIR -Force
  Write-Host "==> Wrote $(Join-Path $OUT_DIR $RepairedWheel.Name)"

  & $EnvPython -m pip install --force-reinstall $RepairedWheel.FullName
  & $EnvPython -c "import ryml; print('Imported ryml from', ryml.__file__)"

  try {
    $Sha = (Get-FileHash -Algorithm SHA256 $RepairedWheel.FullName).Hash
    Write-Host "SHA256  $Sha  $($RepairedWheel.Name)"
  } catch { }
}
finally {
  $env:Path = $OriginalPath
  if ($PushedLocation) {
    Pop-Location
  }
  Remove-Item -LiteralPath $BuildRoot -Recurse -Force -ErrorAction SilentlyContinue
}
