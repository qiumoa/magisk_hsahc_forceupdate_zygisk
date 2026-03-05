# HSAHC 强制更新绕过模块（静态补丁挂载）

目标包名：`com.lta.hsahc.aligames`

## 功能说明

这是一个纯 `Magisk` 模块，不依赖 Zygisk / Frida / LSPosed。

模块内置了已静态补丁的 `libil2cpp.so`，开机后通过 `bind mount` 挂载到目标应用真实库路径：

- 源：`/data/adb/modules/hsahc_forceupdate_zygisk/files/libil2cpp_arm64_patched.so`
- 目标：`/data/app/.../com.lta.hsahc.aligames-.../lib/arm64/libil2cpp.so`

当前补丁适配原始库 SHA256：

- `7a9fdc8f621c39246c77cd02d8f291f2f26c6d3cee0da9b1820f67e1d954922b`

## 本地构建（Windows）

执行：

```bat
build_zygisk_module.cmd
```

输出文件：

- `out/hsahc_forceupdate_zygisk_module.zip`
- `out/INSTALL_ME_hsahc_forceupdate_zygisk_module.zip`

## 安装方式

1. 在 Magisk App 中安装模块 zip：
   - 本地构建：安装 `out/INSTALL_ME_hsahc_forceupdate_zygisk_module.zip`
   - GitHub：到仓库 `Releases` 页面，下载 `INSTALL_ME_hsahc_forceupdate_zygisk_module.zip`
2. 不要安装 `Source code (zip)`
3. 确认 Magisk `>= 26.0`
4. 安装完成后重启手机

## 配置

配置文件路径：

- `/data/adb/modules/hsahc_forceupdate_zygisk/config.prop`
- 或 `/data/adb/hsahc_forceupdate_zygisk/config.prop`

示例：

```properties
target_package=com.lta.hsahc.aligames
target_lib_relpath=lib/arm64/libil2cpp.so
retry_count=180
retry_interval_sec=1
strict_sha256_check=1
orig_libil2cpp_sha256=7a9fdc8f621c39246c77cd02d8f291f2f26c6d3cee0da9b1820f67e1d954922b
patched_libil2cpp_sha256=2d611047b02afbfbcfe404518956236223b323ec95b23ad358b5f6cd676e3bd2
```

## 日志

模块日志路径：

- `/data/adb/hsahc_forceupdate_zygisk/static_mount.log`
