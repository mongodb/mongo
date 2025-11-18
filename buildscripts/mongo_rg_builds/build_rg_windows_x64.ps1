 <# build_rg_windows_x64.ps1
   Builds ripgrep (x86_64-pc-windows-msvc) with PCRE2 bundled+static, LTO=fat, codegen-units=1.
   Output: .\dist\rg-windows-x86_64.exe
#>

$ErrorActionPreference = "Stop"

# ---- Config (override via env before running) -------------------------------
$RG_REPO = if ($env:RG_REPO) { $env:RG_REPO } else { "https://github.com/BurntSushi/ripgrep.git" }
$RG_REF  = if ($env:RG_REF)  { $env:RG_REF }  else { "master" }   # or a tag like 15.0.0 / 14.1.0
$OUT_DIR = if ($env:OUT_DIR) { $env:OUT_DIR } else { Join-Path (Get-Location) "dist" }
$CPU_BASE = if ($env:CPU_BASELINE) { $env:CPU_BASELINE } else { "x86-64" }  # portable baseline

New-Item -ItemType Directory -Force -Path $OUT_DIR | Out-Null

Write-Host "==> ripgrep Windows x86_64"
Write-Host "    RG_REF=$RG_REF"
Write-Host "    OUT_DIR=$OUT_DIR"
Write-Host "    CPU_BASE=$CPU_BASE"

# ---- MSVC sanity ------------------------------------------------------------
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  Write-Warning "MSVC (cl.exe) not found in PATH. Run this in 'Developer PowerShell for VS' or install VS Build Tools."
}

# ---- Rust toolchain ---------------------------------------------------------
if (-not (Get-Command rustup -ErrorAction SilentlyContinue)) {
  Write-Host "Installing rustup..."
  Invoke-WebRequest -UseBasicParsing https://win.rustup.rs -OutFile rustup-init.exe
  Start-Process -Wait -NoNewWindow .\rustup-init.exe -ArgumentList "-y --profile minimal"
  $env:Path += ";$([IO.Path]::Combine($env:USERPROFILE,'.cargo','bin'))"
}
rustup toolchain install stable -q | Out-Null
rustup target add x86_64-pc-windows-msvc -q | Out-Null

# ---- Fetch repo -------------------------------------------------------------
if (-not (Test-Path ripgrep)) {
  git clone --depth=1 --branch $RG_REF $RG_REPO ripgrep | Out-Null
}
Set-Location ripgrep

# ---- Build knobs (use Cargo profile for LTO to avoid bitcode conflicts) ----
$env:CARGO_PROFILE_RELEASE_LTO = "fat"
$env:CARGO_PROFILE_RELEASE_CODEGEN_UNITS = "1"
$env:CARGO_PROFILE_RELEASE_PANIC = "abort"

# Bundle & link PCRE2 statically so there’s no external DLL dependency
$env:PCRE2_SYS_BUNDLED = "1"
$env:PCRE2_SYS_STATIC  = "1"

# Conservative CPU baseline for widest compatibility
$env:RUSTFLAGS = "-C target-cpu=$CPU_BASE -C strip=symbols"

# ---- Build (x86_64 only) ---------------------------------------------------
Write-Host "==> Building x86_64-pc-windows-msvc (PCRE2 bundled, LTO=fat)..."
cargo clean
cargo build --release --features pcre2 --target x86_64-pc-windows-msvc

$bin = Join-Path "target\x86_64-pc-windows-msvc\release" "rg.exe"
if (-not (Test-Path $bin)) {
  throw "Build finished but $bin not found."
}

# ---- Copy artifact + quick metadata ----------------------------------------
$out = Join-Path $OUT_DIR "rg-windows-x86_64.exe"
Copy-Item $bin $out -Force
Write-Host "==> Wrote $out"

# Print a quick dependency list if dumpbin exists
if (Get-Command dumpbin.exe -ErrorAction SilentlyContinue) {
  Write-Host "==> dumpbin /dependents:"
  & dumpbin /dependents $out | Select-String -Pattern "Image has the following dependencies|\.dll"
}

# Checksum for release convenience
try {
  $sha = (Get-FileHash -Algorithm SHA256 $out).Hash
  Write-Host "SHA256  $sha  $(Split-Path -Leaf $out)"
} catch { }

Write-Host "Done."
 
