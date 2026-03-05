param(
  [string]$OutDir = "out"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

$OutRoot = Join-Path $Root $OutDir
$StageDir = Join-Path $OutRoot "stage"
$FilesDir = Join-Path $StageDir "files"
$MetaInfSrc = Join-Path $Root "META-INF"
$MetaInfDst = Join-Path $StageDir "META-INF"
$PatchedSo = Join-Path $Root "files/libil2cpp_arm64_patched.so"

Write-Host "[build] root: $Root"

$required = @(
  (Join-Path $Root "module.prop"),
  (Join-Path $Root "customize.sh"),
  (Join-Path $Root "config.prop"),
  (Join-Path $Root "post-fs-data.sh"),
  (Join-Path $Root "service.sh"),
  $PatchedSo,
  (Join-Path $MetaInfSrc "com/google/android/update-binary"),
  (Join-Path $MetaInfSrc "com/google/android/updater-script")
)
foreach ($f in $required) {
  if (!(Test-Path $f)) {
    throw "required file not found: $f"
  }
}

if (Test-Path $StageDir) {
  Remove-Item -Recurse -Force $StageDir
}
New-Item -ItemType Directory -Path $FilesDir -Force | Out-Null

Copy-Item -Force (Join-Path $Root "module.prop") (Join-Path $StageDir "module.prop")
Copy-Item -Force (Join-Path $Root "customize.sh") (Join-Path $StageDir "customize.sh")
Copy-Item -Force (Join-Path $Root "config.prop") (Join-Path $StageDir "config.prop")
Copy-Item -Force (Join-Path $Root "post-fs-data.sh") (Join-Path $StageDir "post-fs-data.sh")
Copy-Item -Force (Join-Path $Root "service.sh") (Join-Path $StageDir "service.sh")
Copy-Item -Force $PatchedSo (Join-Path $FilesDir "libil2cpp_arm64_patched.so")
Copy-Item -Recurse -Force $MetaInfSrc $MetaInfDst

$ZipPath = Join-Path $OutRoot "hsahc_forceupdate_zygisk_module.zip"
$InstallZipPath = Join-Path $OutRoot "INSTALL_ME_hsahc_forceupdate_zygisk_module.zip"
if (Test-Path $ZipPath) {
  Remove-Item -Force $ZipPath
}
if (Test-Path $InstallZipPath) {
  Remove-Item -Force $InstallZipPath
}
New-Item -ItemType Directory -Path $OutRoot -Force | Out-Null

$TarCmd = Get-Command tar -ErrorAction SilentlyContinue
if (-not $TarCmd) {
  throw "tar command not found. Please use Windows 10/11 built-in tar or install bsdtar."
}

Push-Location $StageDir
try {
  & tar -a -cf $ZipPath *
  if ($LASTEXITCODE -ne 0) {
    throw "tar packaging failed with exit code $LASTEXITCODE"
  }
} finally {
  Pop-Location
}

Copy-Item -Force $ZipPath $InstallZipPath

Write-Host "[build] output zip: $ZipPath"
Write-Host "[build] install zip: $InstallZipPath"
