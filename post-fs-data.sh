#!/system/bin/sh

MODDIR=${0%/*}
LOGDIR=/data/adb/hsahc_forceupdate_zygisk
CFG_DATA=$LOGDIR/config.prop

mkdir -p "$LOGDIR"
chmod 0755 "$LOGDIR"

if [ ! -f "$CFG_DATA" ] && [ -f "$MODDIR/config.prop" ]; then
  cp -f "$MODDIR/config.prop" "$CFG_DATA"
fi

touch "$LOGDIR/static_mount.log"
