# HSAHC 强制更新绕过模块（Zygisk）

目标包名：`com.lta.hsahc.aligames`

## 功能说明

这是一个 `Magisk + Zygisk` 原生模块，不依赖 Frida 或 LSPosed。
模块会在目标进程加载 `libil2cpp.so` 后，尝试补丁强更相关方法：

- `IsVersionLessThanTargetVersion` -> 强制返回 `false`
- `VersionCompare` -> 强制返回 `0`
- `ConfirmVersionForceUpdateJumpCallback` -> 空实现（no-op）

## 本地构建（Windows）

1. 安装 Android NDK（建议 r26+）
2. 执行：

```bat
build_zygisk_module.cmd -AndroidNdk "D:/Android/Sdk/ndk/26.3.11579264"
```

输出文件：

- `out/hsahc_forceupdate_zygisk.zip`

## 安装方式

1. 在 Magisk App 中安装模块 zip：
   - 本地构建：安装 `out/hsahc_forceupdate_zygisk.zip`
   - GitHub：到仓库 `Releases` 页面，下载资产 `hsahc_forceupdate_zygisk.zip` 直接安装
2. 不要安装 `Source code (zip)`，那不是模块包
3. 安装完成后重启手机

## 日志查看

Logcat：

```bash
adb logcat | grep hsahc-zygisk
```

文件日志（模块自动写入）：

- `/data/adb/hsahc_forceupdate_zygisk/runtime.log`
- 回退路径：`/data/user/0/com.lta.hsahc.aligames/files/hsahc_zygisk.log`

查看文件日志：

```bash
adb shell su -c "tail -n 200 /data/adb/hsahc_forceupdate_zygisk/runtime.log"
```
