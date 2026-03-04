param(
  [string]$AndroidNdk = "",
  [string]$OutDir = "out"
)

$ErrorActionPreference = "Stop"

function Resolve-NdkPath {
  param([string]$InputPath)

  if ($InputPath -and (Test-Path $InputPath)) {
    return (Resolve-Path $InputPath).Path
  }
  if ($env:ANDROID_NDK_HOME -and (Test-Path $env:ANDROID_NDK_HOME)) {
    return (Resolve-Path $env:ANDROID_NDK_HOME).Path
  }
  if ($env:ANDROID_NDK_ROOT -and (Test-Path $env:ANDROID_NDK_ROOT)) {
    return (Resolve-Path $env:ANDROID_NDK_ROOT).Path
  }
  throw "Android NDK not found. Please pass -AndroidNdk or set ANDROID_NDK_HOME."
}

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$NdkRoot = Resolve-NdkPath -InputPath $AndroidNdk
$NdkBuild = Join-Path $NdkRoot "ndk-build.cmd"
if (!(Test-Path $NdkBuild)) {
  throw "ndk-build.cmd not found in: $NdkRoot"
}

Write-Host "[build] root: $Root"
Write-Host "[build] ndk : $NdkRoot"

$JniDir = Join-Path $Root "module/jni"
Push-Location $JniDir
try {
  & $NdkBuild NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk NDK_APPLICATION_MK=Application.mk -j4
  if ($LASTEXITCODE -ne 0) {
    throw "ndk-build failed with exit code $LASTEXITCODE"
  }
} finally {
  Pop-Location
}

$OutRoot = Join-Path $Root $OutDir
$StageDir = Join-Path $OutRoot "stage"
$ZygiskDir = Join-Path $StageDir "zygisk"
$LibSrc = Join-Path $Root "module/jni/libs/arm64-v8a/libhsahc_zygisk.so"
$LibDst = Join-Path $ZygiskDir "arm64-v8a.so"

if (!(Test-Path $LibSrc)) {
  throw "built library not found: $LibSrc"
}

if (Test-Path $StageDir) {
  Remove-Item -Recurse -Force $StageDir
}
New-Item -ItemType Directory -Path $ZygiskDir -Force | Out-Null

Copy-Item -Force (Join-Path $Root "module.prop") (Join-Path $StageDir "module.prop")
Copy-Item -Force $LibSrc $LibDst

$ZipPath = Join-Path $OutRoot "hsahc_forceupdate_zygisk.zip"
if (Test-Path $ZipPath) {
  Remove-Item -Force $ZipPath
}
New-Item -ItemType Directory -Path $OutRoot -Force | Out-Null

Push-Location $StageDir
try {
  Compress-Archive -Path * -DestinationPath $ZipPath -Force
} finally {
  Pop-Location
}

Write-Host "[build] output zip: $ZipPath"
