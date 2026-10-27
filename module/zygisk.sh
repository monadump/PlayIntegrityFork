#!/system/bin/sh

FLAG="/data/adb/Box-Brain/PIF/stop"
LOG="/sdcard/zygisk.log"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"
}

run_cmd() {
    log "CMD: $*"
    "$@" >> "$LOG" 2>&1
    RET=$?
    log "EXIT: $RET"
    return $RET
}

log "========================================"
log "PIF controller started"
log "Flag: $FLAG"

if [ -f "$FLAG" ]; then
    log "State: PAUSED"
    log "Stop flag exists, applying pause state"

    run_cmd touch "$FLAG"
else
    log "State: RESUMED"
    log "Stop flag does not exist, applying resume state"

    run_cmd rm -f "$FLAG"
fi

log "Restarting GMS/Vending processes"

run_cmd killall -9 com.google.android.gms.unstable
run_cmd am force-stop com.google.android.gms
run_cmd am force-stop com.android.vending

if [ -f "$FLAG" ]; then
    log "Final state: PAUSED"
else
    log "Final state: RESUMED"
fi

log "========================================"
log " "
log " "
