#!/system/bin/sh
# Pixel Canary pif.prop generator script by osm0sis @ xda-developers
# This file is primarily adapted from https://github.com/osm0sis/PlayIntegrityFork
#
# Modified by: https://github.com/MeowDump

MODDIR="/data/adb/modules/playintegrityfix"
AUTOPIF="$MODDIR/autopif4"
BOX_BRAIN="/data/adb/Box-Brain"
PIF_DIR="$BOX_BRAIN/PIF"

FLAG_CUSTOM_PIF="$PIF_DIR/custom-pif"
FLAG_SKIP_JSON="$PIF_DIR/skip_json"
CUSTOM_PIF_SCRIPT="$MODDIR/webroot/common_scripts/custom_pif.sh"
MEOW_JSON="/sdcard/meow.json"

URL_ANDROID="https://developer.android.com"
URL_VERSIONS="$URL_ANDROID/about/versions"
URL_FLASH="https://flash.android.com/"
URL_STATION="https://content-flashstation-pa.googleapis.com/v1/builds"

item() { printf ' ✦ %s\n' "$*"; }
die()  { printf ' ✖ %s\n' "$*" >&2; exit 1; }
warn() { printf ' 😭 %s\n' "$*"; }

if [ -f "$FLAG_CUSTOM_PIF" ]; then
  sh "$CUSTOM_PIF_SCRIPT";
  exit 0;
fi;

# Load shared downloader utilities from the module's common scripts.
MODPATH="${0%/*}"
COMMON_FUNC="$MODPATH/common_func.sh"
[ -f "$COMMON_FUNC" ] || COMMON_FUNC="$MODDIR/webroot/common_scripts/common_func.sh"
[ -f "$COMMON_FUNC" ] && . "$COMMON_FUNC"

# Android environments may not ship curl/wget. Explicitly detect APatch's BusyBox
# and invoke BusyBox applets directly when standalone downloaders are unavailable.
PIXEL_BB=""
for _bb in /data/adb/ap/bin/busybox /data/adb/ksu/bin/busybox \
           /data/adb/magisk/busybox /data/adb/modules/busybox-ndk/system/*/busybox; do
  [ -x "$_bb" ] && PIXEL_BB="$_bb" && break
 done

fetch_file() {
  OUT="$1"; URL="$2"; REFERER="$3"
  TMP="${OUT}.tmp.$$"
  rm -f "$OUT" "$TMP"
  ATTEMPT=1
  while [ "$ATTEMPT" -le 5 ]; do
    [ "$ATTEMPT" -gt 1 ] && item "Retrying... $((ATTEMPT - 1))/5"

    # curl: normal connection, then IPv4 as a fallback.
    if command -v curl >/dev/null 2>&1; then
      if [ -n "$REFERER" ]; then
        curl -fsSL --connect-timeout 20 --max-time 90 --http1.1 -A 'Mozilla/5.0' -H "Referer: $REFERER" "$URL" -o "$TMP" 2>/dev/null
      else
        curl -fsSL --connect-timeout 20 --max-time 90 --http1.1 -A 'Mozilla/5.0' "$URL" -o "$TMP" 2>/dev/null
      fi
      [ $? -eq 0 ] && [ -s "$TMP" ] && mv -f "$TMP" "$OUT" && return 0
      rm -f "$TMP"
      if [ -n "$REFERER" ]; then
        curl -4 -fsSL --connect-timeout 20 --max-time 90 -A 'Mozilla/5.0' -H "Referer: $REFERER" "$URL" -o "$TMP" 2>/dev/null
      else
        curl -4 -fsSL --connect-timeout 20 --max-time 90 -A 'Mozilla/5.0' "$URL" -o "$TMP" 2>/dev/null
      fi
      [ $? -eq 0 ] && [ -s "$TMP" ] && mv -f "$TMP" "$OUT" && return 0
      rm -f "$TMP"
    fi

    # Standalone wget, if available.
    if command -v wget >/dev/null 2>&1; then
      if [ -n "$REFERER" ]; then
        wget -T 30 -t 1 -O "$TMP" --header="Referer: $REFERER" --user-agent='Mozilla/5.0' "$URL" 2>/dev/null
      else
        wget -T 30 -t 1 -O "$TMP" --user-agent='Mozilla/5.0' "$URL" 2>/dev/null
      fi
      [ $? -eq 0 ] && [ -s "$TMP" ] && mv -f "$TMP" "$OUT" && return 0
      rm -f "$TMP"
    fi

    # BusyBox wget fallback (notably APatch's /data/adb/ap/bin/busybox).
    if [ -n "$PIXEL_BB" ]; then
      if [ -n "$REFERER" ]; then
        "$PIXEL_BB" wget -T 30 -t 1 -O "$TMP" --header="Referer: $REFERER" --user-agent='Mozilla/5.0' "$URL" 2>/dev/null
      else
        "$PIXEL_BB" wget -T 30 -t 1 -O "$TMP" --user-agent='Mozilla/5.0' "$URL" 2>/dev/null
      fi
      [ $? -eq 0 ] && [ -s "$TMP" ] && mv -f "$TMP" "$OUT" && return 0
      rm -f "$TMP"
    fi

    # Toybox wget fallback, if present and functional.
    if toybox wget --help >/dev/null 2>&1; then
      if [ -n "$REFERER" ]; then
        toybox wget -T 30 -O "$TMP" --header="Referer: $REFERER" "$URL" 2>/dev/null
      else
        toybox wget -T 30 -O "$TMP" "$URL" 2>/dev/null
      fi
      [ $? -eq 0 ] && [ -s "$TMP" ] && mv -f "$TMP" "$OUT" && return 0
      rm -f "$TMP"
    fi

    [ "$ATTEMPT" -lt 5 ] && sleep "$ATTEMPT"
    ATTEMPT=$((ATTEMPT + 1))
  done
  rm -f "$OUT" "$TMP"
  return 1
}

mkdir -p "$AUTOPIF";
cd "$AUTOPIF" || die "Cannot access $AUTOPIF";

item "Fetching Pixel device list...";
fetch_file PIXEL_VERSIONS_HTML "$URL_VERSIONS" "" || die "Failed to download Pixel device list";
LATEST_BETA=$(grep -B4 -A2 'data-icon="preview' PIXEL_VERSIONS_HTML | grep -o 'href="/about/versions/.*[0-9]"' | cut -d\" -f2);
[ "$LATEST_BETA" ] || LATEST_BETA=$(grep -oE 'href="/about/versions/[0-9]{2}"' PIXEL_VERSIONS_HTML | cut -d\" -f2 | sort -ru | head -n1);
fetch_file PIXEL_LATEST_HTML "$URL_ANDROID$LATEST_BETA" "" || die "Failed to download latest Pixel version page";
fetch_file PIXEL_FI_HTML "$URL_ANDROID$(grep -o 'href=".*download.*"' PIXEL_LATEST_HTML | grep -v 'ota' | cut -d\" -f2 | sort -ru | head -n1)" "" || die "Failed to download factory image page";
fetch_file PIXEL_OTA_HTML "$URL_ANDROID$(grep -o 'href=".*download-ota.*"' PIXEL_LATEST_HTML | cut -d\" -f2 | sort -ru | head -n1)" "" || die "Failed to download OTA page";
SRC=FI; [ "$(grep 'tr id=' PIXEL_FI_HTML | sed 's;.*<tr id="\(.*\)">.*;\1;' | wc -w)" -lt "$(grep 'tr id=' PIXEL_OTA_HTML | sed 's;.*<tr id="\(.*\)">.*;\1;' | wc -w)" ] && SRC=OTA;
MODEL_LIST="$(grep -A1 'tr id=' PIXEL_${SRC}_HTML | grep 'td' | sed 's;.*<td>\(.*\)</td>.*;\1;')";
PRODUCT_LIST="$(grep 'tr id=' PIXEL_${SRC}_HTML | sed 's;.*<tr id="\(.*\)">.*;\1_beta;')";
[ -n "$PRODUCT_LIST" ] || die "Pixel device list is empty";

# Device pin
for PROD in $PRODUCT_LIST; do
  DEV_FLAG_CANDIDATE="${PROD%_beta}";
  if [ -f "$PIF_DIR/$DEV_FLAG_CANDIDATE" ]; then
    DEVICE_REQUEST="$DEV_FLAG_CANDIDATE";
    item "Requested device prop: $DEV_FLAG_CANDIDATE";
    break;
  fi;
done;

item "Selecting Pixel build...";

select_requested_device() {
  local query="$1" lquery list_count idx model prod dev lmodel ldev;
  lquery="$(echo "$query" | tr '[:upper:]' '[:lower:]')";
  lquery="${lquery%_beta}";
  list_count="$(echo "$MODEL_LIST" | wc -l)";
  idx=1;
  while [ "$idx" -le "$list_count" ]; do
    local IFS=$'\n';
    set -- $MODEL_LIST;
    model="$(eval echo \${$idx})";
    set -- $PRODUCT_LIST;
    prod="$(eval echo \${$idx})";
    dev="$(echo "$prod" | sed 's/_beta$//')";
    lmodel="$(echo "$model" | tr '[:upper:]' '[:lower:]')";
    ldev="$(echo "$dev" | tr '[:upper:]' '[:lower:]')";
    if [ "$ldev" = "$lquery" ]; then
      MODEL="$model"; PRODUCT="$prod"; DEVICE="$dev";
      return 0;
    fi;
    case "$lmodel" in
      *"$lquery"*) MODEL="$model"; PRODUCT="$prod"; DEVICE="$dev"; return 0;;
    esac;
    idx=$((idx + 1));
  done;
  return 1;
}
if [ -n "$DEVICE_REQUEST" ]; then
  if select_requested_device "$DEVICE_REQUEST"; then
    item "Matched requested device: $MODEL ($DEVICE)";
  else
    warn "Requested device '$DEVICE_REQUEST' not found in the current Pixel beta list";
  fi;
fi;

set_random_beta() {
  local list_count="$(echo "$MODEL_LIST" | wc -l)";
  local list_rand="$((RANDOM % $list_count + 1))";
  local IFS=$'\n';
  set -- $MODEL_LIST;
  MODEL="$(eval echo \${$list_rand})";
  set -- $PRODUCT_LIST;
  PRODUCT="$(eval echo \${$list_rand})";
  DEVICE="$(echo "$PRODUCT" | sed 's/_beta//')";
}
[ -n "$PRODUCT" ] || set_random_beta;
item "Selected $MODEL";
printf '\n';

get_canary_info() {
  item "Fetching build metadata...";
  fetch_file PIXEL_FLASH_HTML "$URL_FLASH" "" || die "Failed to download Flash Android page";
  fetch_file PIXEL_STATION_JSON "$URL_STATION?product=$PRODUCT&key=$(grep -o '<body data-client-config=.*' PIXEL_FLASH_HTML | cut -d\; -f2 | cut -d\& -f1)" "$URL_FLASH" || die "Failed to download Pixel build metadata";
  tac PIXEL_STATION_JSON | grep -m1 -A13 '"canary": true' > PIXEL_CANARY_JSON;
  ID="$(grep 'releaseCandidateName' PIXEL_CANARY_JSON | cut -d\" -f4)";
  INCREMENTAL="$(grep 'buildId' PIXEL_CANARY_JSON | cut -d\" -f4)";
}
get_canary_info;
if [ -z "$ID" -o -z "$INCREMENTAL" ] && [ -n "$DEVICE_REQUEST" ]; then
  warn "Failed to extract matching build info from JSON";
  item "Retrying with another Pixel build...";
  set_random_beta;
  item "Selected $MODEL";
  get_canary_info;
fi;
[ -z "$ID" -o -z "$INCREMENTAL" ] && die "Failed to extract build info from JSON";

FI="$(grep 'factoryImageDownloadUrl' PIXEL_CANARY_JSON | cut -d\" -f4)";
FI_HOST="$(echo "$FI" | sed 's;^.*://\(.*\)$;\1;' | cut -d/ -f1)";
FI_PATH="/$(echo "$FI" | sed 's;^.*://\(.*\)$;\1;' | cut -d/ -f2-)";
if [ "$FI" -a "$FI_HOST" -a "$FI_PATH" ]; then
  nc $FI_HOST 80 <<EOF | tr -d '\r' > PIXEL_ZIP_HEADERS;
HEAD $FI_PATH HTTP/1.1
Host: $FI_HOST
Connection: close

EOF
else
  warn "Failed to extract Factory Image URL from JSON";
fi;
if [ ! -s PIXEL_ZIP_HEADERS ] || ! grep -q 'Last-Modified' PIXEL_ZIP_HEADERS; then
  wget -q -T 10 -S --spider -o PIXEL_ZIP_HEADERS --no-check-certificate "$FI" 2>&1;
fi;
if [ -f PIXEL_ZIP_HEADERS ] && grep -q 'Last-Modified' PIXEL_ZIP_HEADERS; then
  CANARY_REL_DATE="$(date -D '%a, %d %b %Y %H:%M:%S %Z' -d "$(grep -o 'Last-Modified.*' PIXEL_ZIP_HEADERS | cut -d\  -f2-)" '+%Y-%m-%d')";
  CANARY_EXP_DATE="$(date -D '%s' -d "$(($(date -D '%Y-%m-%d' -d "$CANARY_REL_DATE" '+%s') + 60 * 60 * 24 * 7 * 6))" '+%Y-%m-%d')";
  item "Build metadata: $CANARY_REL_DATE ·";
  item "Build expiry $CANARY_EXP_DATE";
else
  warn "Failed to determine Release Date from HTTP headers";
  CANARY_REL_DATE="Unknown";
  CANARY_EXP_DATE="Unknown";
fi;

printf '\n';
item "Applying security patch";
PATCH_FILE="$BOX_BRAIN/patch.txt";
SECURITY_PATCH="2026-11-05";

if [ -f "$PATCH_FILE" ]; then
  PATCH_VALUE="$(cat "$PATCH_FILE" 2>/dev/null)";
  [ -n "$PATCH_VALUE" ] && SECURITY_PATCH="$PATCH_VALUE";
fi;

item "Resolving API level...";
case $DEVICE in
  bluejay) DEVICE_INITIAL_SDK_INT=32;; # Pixel 6a
  panther|cheetah) DEVICE_INITIAL_SDK_INT=33;; # Pixel 7 Series
  lynx) DEVICE_INITIAL_SDK_INT=33;; # Pixel 7a
  felix) DEVICE_INITIAL_SDK_INT=33;; # Pixel Fold
  tangorpro) DEVICE_INITIAL_SDK_INT=33;; # Pixel Tablet
  shiba|husky) DEVICE_INITIAL_SDK_INT=34;; # Pixel 8 Series
  akita) DEVICE_INITIAL_SDK_INT=34;; # Pixel 8a
  tokay|caiman|komodo|comet) DEVICE_INITIAL_SDK_INT=34;; # Pixel 9 Series
  tegu) DEVICE_INITIAL_SDK_INT=35;; # Pixel 9a
  frankel|blazer|mustang|rango) DEVICE_INITIAL_SDK_INT=36;; # Pixel 10 Series
  stallion) DEVICE_INITIAL_SDK_INT=35;; # Pixel 10a; Actual 36
  cubs|grizzly|kodiak|yogi) DEVICE_INITIAL_SDK_INT=37;; # Pixel 11 Series
  *) DEVICE_INITIAL_SDK_INT=37;;
esac;

ADVSETTINGS="spoofBuild spoofProps spoofApps spoofPixel spoofPhotos spoofPatch spoofProvider spoofSignature spoofVendingFinger spoofVendingSdk verboseLogs";
spoofBuild=1; spoofProps=1; spoofApps=0; spoofPixel=0; spoofPhotos=0;
spoofPatch=0; spoofProvider=0; spoofSignature=0; spoofVendingFinger=0;
spoofVendingSdk=0; verboseLogs=0;

EXISTING="$MODDIR/custom.pif.prop";
if [ -f "$EXISTING" ] && grep -q '^verboseLogs=' "$EXISTING"; then
  item "Retaining existing Advanced Settings...";
  for SETTING in $ADVSETTINGS; do
    if grep -q "^$SETTING=" "$EXISTING"; then
      VALUE="$(grep -m1 "^$SETTING=" "$EXISTING" | cut -d= -f2 | cut -d\# -f1 | sed 's/[[:space:]]*$//')";
      [ -n "$VALUE" ] && eval "$SETTING=\"\$VALUE\"";
    fi;
  done;
fi;

printf '\n';
item "Building Pixel config...";
MANUFACTURER=Google;
BRAND=google;
RELEASE=CANARY;
TYPE=user;
TAGS=release-keys;
FINGERPRINT="google/$PRODUCT/$DEVICE:CANARY/$ID/$INCREMENTAL:user/release-keys";
ALLFIELDS="MANUFACTURER MODEL FINGERPRINT BRAND PRODUCT DEVICE RELEASE ID INCREMENTAL TYPE TAGS SECURITY_PATCH DEVICE_INITIAL_SDK_INT";
RUN_DATE="$(date '+%Y-%m-%d')";

{
  for FIELD in $ALLFIELDS; do
    eval VALUE=\$$FIELD;
    printf '%s=%s\n' "$FIELD" "$VALUE";
  done;
  printf '\n# Configuration\n';
  for SETTING in $ADVSETTINGS; do
    eval VALUE=\$$SETTING;
    printf '%s=%s\n' "$SETTING" "$VALUE";
  done;
  printf '\n# Canary Released: %s\n' "${CANARY_REL_DATE:-Unknown}";
  printf '# Estimated Expiry: %s\n' "${CANARY_EXP_DATE:-Unknown}";
  printf '# Generated by Integrity Box on %s\n' "$RUN_DATE";
} > custom.pif.prop;

if [ ! -f "$FLAG_SKIP_JSON" ]; then
  json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
  TOTAL="$(echo "$ALLFIELDS" | wc -w)";
  COUNT=1;
  {
    printf '{\n';
    printf '  // Generated by Integrity Box on %s\n' "$RUN_DATE";
    for FIELD in $ALLFIELDS; do
      eval VALUE=\$$FIELD;
      SEP=","; [ "$COUNT" -eq "$TOTAL" ] && SEP="";
      printf '  "%s": "%s"%s\n' "$FIELD" "$(json_escape "$VALUE")" "$SEP";
      COUNT=$((COUNT + 1));
    done;
    printf '}\n';
  } > "$MEOW_JSON";
  printf '\n';
  item "Exported $MEOW_JSON";
fi;

item "Installing config...";
if [ -f "$MODDIR/custom.pif.prop" ]; then
  item "Backing up previous config...";
  mv -f "$MODDIR/custom.pif.prop" "$MODDIR/custom.pif.prop.bak";
fi;
cp -f custom.pif.prop "$MODDIR/custom.pif.prop";

if [ "$DEVICE_INITIAL_SDK_INT" -gt 35 ]; then
  echo "----------------------------------------"
  echo " ⚠ This Fingerprint may require "
  echo "    an RKP keybox to pass integrity."
  echo "----------------------------------------"
fi;

printf '\n';

update_module_description