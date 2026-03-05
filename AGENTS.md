# Repository Guidelines

## Project Structure & Module Organization
This repository packages a Magisk module that mounts a patched `libil2cpp.so` for `com.lta.hsahc.aligames`.

- `module/jni/`: Zygisk native source (`hsahc_module.cpp`) and NDK makefiles.
- `files/`: Runtime payload, including `libil2cpp_arm64_patched.so`.
- `META-INF/com/google/android/`: Recovery installer scripts (`update-binary`, `updater-script`).
- Root scripts: `customize.sh`, `post-fs-data.sh`, `service.sh`, `config.prop`, `module.prop`.
- Tooling: `tools/patch_libil2cpp.py` for static patch generation.

## Build, Test, and Development Commands
- `python tools/patch_libil2cpp.py --input <orig.so> --output files/libil2cpp_arm64_patched.so`  
  Rebuilds the patched IL2CPP binary.
- `./gradlew :module:assembleDebug`  
  Builds the native module project (requires Android SDK/NDK).
- `bash -lc "mkdir -p out/stage/files out/stage/META-INF && cp ..."`  
  Package flow follows `.github/workflows/build-zygisk.yml` and produces:
  `out/hsahc_forceupdate_zygisk_module.zip` and `out/INSTALL_ME_hsahc_forceupdate_zygisk_module.zip`.
- `unzip -l out/hsahc_forceupdate_zygisk_module.zip` and `sha256sum out/*.zip`  
  Validate archive contents and checksums before release.

## Coding Style & Naming Conventions
- C++: use C++17 (see `Application.mk`), 2-space indentation, `kCamelCase` constants, `snake_case` config keys.
- Shell scripts: POSIX `sh`, keep logic idempotent and boot-safe.
- Python tooling: keep functions small and deterministic; prefer explicit CLI args.
- Do not hardcode new package names/paths; expose through `config.prop` when possible.

## Testing Guidelines
There is no formal unit-test suite yet. Every change must include:
- Build verification (`:module:assembleDebug` when native code changes).
- Package integrity checks (`unzip -l`, checksum generation).
- Device-side smoke test: install module, reboot, and confirm logs under `/data/adb/hsahc_forceupdate_zygisk/`.

## Commit & Pull Request Guidelines
- Follow Conventional Commits as used in history: `fix:`, `feat:`, `ci:`, `refactor:`.
- Keep subject lines concise and scoped to one change.
- PRs should include: purpose, changed files/paths, validation steps, and key log snippets.
- For release-impacting updates, attach new SHA256 values and note any `config.prop` defaults changed.

## Security & Configuration Tips
- Treat patched binaries and hashes as coupled artifacts; update both together.
- Review script permissions (`0755` for executable installer scripts) before publishing.
