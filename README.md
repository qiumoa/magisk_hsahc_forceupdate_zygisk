# HSAHC 强更绕过模块（Zygisk）

目标包名：`com.lta.hsahc.aligames`

## 方案说明

这是 `Magisk + Zygisk` 原生模块，不依赖 Frida 和 LSPosed。  
模块会等待 `libil2cpp.so` 加载后，直接补丁强更判定方法：

- `IsVersionLessThanTargetVersion` -> 强制返回 `false`
- `VersionCompare` -> 强制返回 `0`
- `ConfirmVersionForceUpdateJumpCallback` -> 空实现

优先在类名包含 `GameMgr` 的方法上打补丁；若未命中，会回退到更宽松的匹配。

## 本地构建（Windows）

1. 安装 Android NDK（r21+，建议 r26+）
2. 执行：

```bat
build_zygisk_module.cmd -AndroidNdk "D:/Android/Sdk/ndk/26.3.11579264"
```

输出文件：

- `out/hsahc_forceupdate_zygisk.zip`

## 安装方式

1. 在 Magisk App 中安装 `out/hsahc_forceupdate_zygisk.zip`
2. 重启手机

## 日志查看

Logcat 关键字：

```bash
adb logcat | grep hsahc-zygisk
```

手机文件日志（模块会自动写）：

- `/data/adb/hsahc_forceupdate_zygisk/runtime.log`
- 回退路径：`/data/user/0/com.lta.hsahc.aligames/files/hsahc_zygisk.log`

查看文件日志：

```bash
adb shell su -c "tail -n 200 /data/adb/hsahc_forceupdate_zygisk/runtime.log"
```

## 预期日志

- `命中目标进程，开始准备 il2cpp 补丁`
- `已补丁 ... IsVersionLessThanTargetVersion`
- `已补丁 ... VersionCompare`
- `il2cpp 强更绕过已生效`

若出现 `在超时窗口内未能补丁 il2cpp 目标方法`，说明该版本方法名或结构有变化，需要继续做偏移级 Hook。
