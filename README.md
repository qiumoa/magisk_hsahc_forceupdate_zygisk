# HSAHC ForceUpdate Bypass (Zygisk)

Target package: `com.lta.hsahc.aligames`

This is a Magisk + Zygisk native module (no Frida, no LSPosed dependency).
It waits for `libil2cpp.so` and patches force-update decision methods:

- `IsVersionLessThanTargetVersion` -> return `false`
- `VersionCompare` -> return `0`
- `ConfirmVersionForceUpdateJumpCallback` -> no-op

It first patches methods in classes containing `GameMgr`.
If no hit is found, it falls back to a wider name-only match.

## Build on Windows

1. Install Android NDK (r21+, recommended r26+).
2. Run:

```bat
build_zygisk_module.cmd -AndroidNdk "D:/Android/Sdk/ndk/26.3.11579264"
```

Output:

- `out/hsahc_forceupdate_zygisk.zip`

## Install

Install the zip in Magisk App, then reboot.

## Logs

```bash
adb logcat | grep hsahc-zygisk
```

Expected logs:

- `target app matched, preparing il2cpp patch`
- `patched ... IsVersionLessThanTargetVersion`
- `patched ... VersionCompare`
- `il2cpp force-update bypass active`

If you see `failed to patch il2cpp methods within timeout`, method names/layout changed in this game version and an offset-level hook will be needed.
