#!/system/bin/sh
# Runs inside the KernelSU / KernelSU Next installer.
SKIPUNZIP=0

CFG_DIR=/data/adb/gpu_spoofer
CFG_FILE=$CFG_DIR/config.txt

ui_print "- Per-Game GPU Spoofer"
ui_print "- Preparing $CFG_DIR"
mkdir -p "$CFG_DIR"
if [ ! -f "$CFG_FILE" ]; then
  printf '# pkg|renderer|vendor|hw|plat|soc\n' > "$CFG_FILE"
fi
chmod 755 "$CFG_DIR"
chmod 644 "$CFG_FILE"
ui_print "- Done. Enable Zygisk, reboot, then open the module WebUI."
