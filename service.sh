#!/system/bin/sh

MODDIR=${0%/*}
LOGDIR=/data/adb/hsahc_forceupdate_zygisk
LOGFILE=$LOGDIR/static_mount.log
CFG_MODULE=$MODDIR/config.prop
CFG_DATA=$LOGDIR/config.prop
PATCH_SO=$MODDIR/files/libil2cpp_arm64_patched.so

mkdir -p "$LOGDIR"

logi() {
  echo "$(date '+%Y-%m-%d %H:%M:%S') [INFO] $1" >>"$LOGFILE"
}

loge() {
  echo "$(date '+%Y-%m-%d %H:%M:%S') [ERROR] $1" >>"$LOGFILE"
}

read_cfg() {
  key="$1"
  file="$2"
  [ -f "$file" ] || return 0
  sed -n "s/^${key}=//p" "$file" | head -n 1
}

cfg_get() {
  key="$1"
  def="$2"
  val="$(read_cfg "$key" "$CFG_DATA")"
  [ -n "$val" ] || val="$(read_cfg "$key" "$CFG_MODULE")"
  [ -n "$val" ] || val="$def"
  echo "$val"
}

TARGET_PACKAGE="$(cfg_get target_package com.lta.hsahc.aligames)"
TARGET_LIB_RELPATH="$(cfg_get target_lib_relpath lib/arm64/libil2cpp.so)"
RETRY_COUNT="$(cfg_get retry_count 180)"
RETRY_INTERVAL_SEC="$(cfg_get retry_interval_sec 1)"
STRICT_SHA256_CHECK="$(cfg_get strict_sha256_check 1)"
ORIG_SHA256="$(cfg_get orig_libil2cpp_sha256 "")"
PATCHED_SHA256="$(cfg_get patched_libil2cpp_sha256 "")"

if [ ! -f "$PATCH_SO" ]; then
  loge "patched so missing: $PATCH_SO"
  exit 1
fi

if ! command -v sha256sum >/dev/null 2>&1; then
  loge "sha256sum not found"
  exit 1
fi

APK_PATH="$(pm path "$TARGET_PACKAGE" 2>/dev/null | sed -n 's/^package://p' | head -n 1)"
if [ -z "$APK_PATH" ]; then
  loge "target package not found: $TARGET_PACKAGE"
  exit 1
fi

APP_DIR="$(dirname "$APK_PATH")"
TARGET_SO="$APP_DIR/$TARGET_LIB_RELPATH"
logi "module start: package=$TARGET_PACKAGE target=$TARGET_SO"

i=0
while [ "$i" -lt "$RETRY_COUNT" ]; do
  if [ ! -f "$TARGET_SO" ]; then
    if [ $((i % 10)) -eq 0 ]; then
      logi "waiting target so... retry=$i"
    fi
    i=$((i + 1))
    sleep "$RETRY_INTERVAL_SEC"
    continue
  fi

  target_hash="$(sha256sum "$TARGET_SO" | awk '{print $1}')"
  patch_hash="$(sha256sum "$PATCH_SO" | awk '{print $1}')"

  if [ "$target_hash" = "$patch_hash" ]; then
    logi "target already patched"
    exit 0
  fi

  if [ "$STRICT_SHA256_CHECK" = "1" ]; then
    if [ -n "$ORIG_SHA256" ] && [ "$target_hash" != "$ORIG_SHA256" ]; then
      loge "target hash mismatch: got=$target_hash expect=$ORIG_SHA256"
      exit 1
    fi
    if [ -n "$PATCHED_SHA256" ] && [ "$patch_hash" != "$PATCHED_SHA256" ]; then
      loge "patch hash mismatch: got=$patch_hash expect=$PATCHED_SHA256"
      exit 1
    fi
  fi

  if mount -o bind "$PATCH_SO" "$TARGET_SO"; then
    new_hash="$(sha256sum "$TARGET_SO" | awk '{print $1}')"
    if [ "$new_hash" = "$patch_hash" ]; then
      logi "bind mount success"
      exit 0
    fi
    loge "verify failed after mount: target_hash=$new_hash patch_hash=$patch_hash"
  else
    loge "bind mount failed"
  fi

  i=$((i + 1))
  sleep "$RETRY_INTERVAL_SEC"
done

loge "max retries reached"
exit 1
