# HSAHC 强制更新绕过模块

目标包名：`com.lta.hsahc.aligames`

## 功能说明

纯 Magisk 模块，通过 bind mount 挂载已补丁的 `libil2cpp.so` 绕过强制更新检查。

- 源：`/data/adb/modules/hsahc_forceupdate_zygisk/files/libil2cpp_arm64_patched.so`
- 目标：`/data/app/.../com.lta.hsahc.aligames-.../lib/arm64/libil2cpp.so`

适配原始库 SHA256：`7a9fdc8f621c39246c77cd02d8f291f2f26c6d3cee0da9b1820f67e1d954922b`

## 构建

通过 GitHub Actions 自动构建，在 Releases 页面下载 `INSTALL_ME_*.zip`

## 安装

1. 在 Magisk 中安装 `INSTALL_ME_*.zip`
2. 重启设备

## 配置

编辑 `/data/adb/hsahc_forceupdate_zygisk/config.prop`

## 日志

查看 `/data/adb/hsahc_forceupdate_zygisk/static_mount.log`
