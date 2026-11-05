#!/usr/bin/env bash
# Build, package, upload and VERIFY a DKR PS4 release.
#
# Why the checks exist: every silent failure in this chain once cost a full
# console install cycle. PkgTool.Core aborts (SIGABRT) when its output
# directory is missing or when the native linux build cannot find libssl, and
# a stale .pkg then looks like "the fix did nothing". Pipelines through tail
# hid that for several builds.
#
# Usage:  ./release.sh 04.00                    build + package only
#         PS4_IP=<console ip> ./release.sh 04.00  also install on the console
# Installing needs GoldHEN's FTP server (port 2121) and the Remote Package
# Installer app open on the console (port 12800). LAN_IP (this computer's
# address as seen by the console) is detected automatically.
set -euo pipefail

VER="${1:?usage: release.sh APP_VER (e.g. 01.23)}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/build-ps4"
PKG_NAME="UP0001-CUSA64004_00-0000000000000001.pkg"
CONSOLE="${PS4_IP:-}"

cd "$ROOT"

echo "== 1/5 build (APP_VER $VER) =="
export APP_VER="$VER"
./platform/ps4/build_ps4.sh
[[ -f "$OUT/eboot.bin" ]] || { echo "FAIL: no eboot.bin"; exit 1; }

echo "== 2/5 package (APP_VER $VER) =="
python3 platform/ps4/make_pkg.py \
    --template "$HERE/sce_sys_template" \
    --eboot "$OUT/eboot.bin" --out "$OUT/pkg" --app-ver "$VER"
[[ -f "$OUT/pkg/$PKG_NAME" ]] || { echo "FAIL: package missing"; exit 1; }

LOCAL_SHA="$(sha256sum "$OUT/pkg/$PKG_NAME" | cut -d' ' -f1)"
echo "   local sha256: $LOCAL_SHA"

cp "$OUT/pkg/$PKG_NAME" "$OUT/DKR-$VER.pkg"
echo "   package: $OUT/DKR-$VER.pkg"
if [[ -z "$CONSOLE" ]]; then
    echo "== done (set PS4_IP=<console ip> to install on the console as well) =="
    exit 0
fi
LAN_IP="${LAN_IP:-$(ip -4 route get "$CONSOLE" | sed -n 's/.* src \([0-9.]*\).*/\1/p')}"

echo "== 3/5 stage on console =="
timeout 300 curl -sS --connect-timeout 10 \
    "ftp://$CONSOLE:2121/data/pkg/" -T "$OUT/pkg/$PKG_NAME"

echo "== 4/5 remote install (Remote Package Installer) =="
RPI="http://$CONSOLE:12800/api"
if curl -sS --max-time 5 -X POST -d '{"title_id":"CUSA64004"}' "$RPI/is_exists" \
        | grep -q '"status": *"success"'; then
    # Reuse an already running server on this port instead of fighting for it.
    if curl -sS -r 0-15 -o /dev/null --max-time 3 "http://$LAN_IP:8421/$PKG_NAME"; then
        echo "   reusing the HTTP server on $LAN_IP:8421"
    else
        python3 "$ROOT/platform/ps4/pkgserve.py" "$OUT/pkg" 8421 "$LAN_IP" \
            >"$OUT/pkgserve.log" 2>&1 &
        SERVE_PID=$!
        trap 'kill $SERVE_PID 2>/dev/null' EXIT
        sleep 1
    fi
    RESP="$(curl -sS --max-time 60 -X POST \
        -d "{\"type\":\"direct\",\"packages\":[\"http://$LAN_IP:8421/$PKG_NAME\"]}" \
        "$RPI/install")"
    echo "   $RESP"
    if echo "$RESP" | grep -q '0x80990015'; then
        # SCE_BGFT_ERROR_TASK_DUPLICATED: an install task for this content is
        # still queued from an earlier attempt. Clear it, uninstall, retry.
        echo "   duplicate install task — clearing"
        TID="$(curl -sS --max-time 15 -X POST \
            -d "{\"content_id\":\"UP0001-CUSA64004_00-0000000000000001\",\"sub_type\":6}" \
            "$RPI/find_task" | sed -n 's/.*"task_id": *\([0-9]*\).*/\1/p')"
        if [ -n "$TID" ]; then
            curl -sS --max-time 15 -X POST -d "{\"task_id\":$TID}" \
                "$RPI/unregister_task" >/dev/null
        fi
        curl -sS --max-time 30 -X POST -d '{"title_id":"CUSA64004"}' \
            "$RPI/uninstall_game" >/dev/null
        sleep 5
        RESP="$(curl -sS --max-time 60 -X POST \
            -d "{\"type\":\"direct\",\"packages\":[\"http://$LAN_IP:8421/$PKG_NAME\"]}" \
            "$RPI/install")"
        echo "   retry: $RESP"
    fi
    echo "$RESP" | grep -q '"status": *"success"' || {
        echo "FAIL: installer rejected the package"; exit 1; }
    # Report the install itself as soon as the console confirms the transfer;
    # the hash check below reads the whole package back and takes ~30 s, which
    # must not look like a slow install.
    TASK_ID="$(echo "$RESP" | sed -n 's/.*"task_id": *\([0-9]*\).*/\1/p')"
    if [ -n "$TASK_ID" ]; then
        for _ in $(seq 1 20); do
            PROG="$(curl -sS --max-time 10 -X POST -d "{\"task_id\":$TASK_ID}" \
                "$RPI/get_task_progress" 2>/dev/null)"
            TR="$(echo "$PROG" | sed -n 's/.*"transferred": *\(0x[0-9A-Fa-f]*\).*/\1/p')"
            LN="$(echo "$PROG" | sed -n 's/.*"length": *\(0x[0-9A-Fa-f]*\).*/\1/p')"
            if [ -n "$TR" ] && [ -n "$LN" ] && [ "$TR" = "$LN" ]; then
                echo "   INSTALLED (task $TASK_ID, transfer complete)"
                break
            fi
            sleep 1
        done
    fi
    for _ in $(seq 1 24); do
        sleep 3
        PROG="$(curl -sS --max-time 10 -X POST -d "$(echo "$RESP" \
            | sed -n 's/.*\"task_id\": *\([0-9]*\).*/{\"task_id\":\1}/p')" \
            "$RPI/get_task_progress" 2>/dev/null || true)"
        echo "$PROG" | grep -q '"transferred": *"\(0x[0-9A-Fa-f]*\)"' || continue
        [[ "$(echo "$PROG" | sed -n 's/.*"transferred": *\(0x[0-9A-Fa-f]*\).*/\1/p')" \
           == "$(echo "$PROG" | sed -n 's/.*"length": *\(0x[0-9A-Fa-f]*\).*/\1/p')" ]] && break
    done
else
    echo "   Remote installer not answering (app must be in the foreground)."
    echo "   Package staged at /data/pkg/$PKG_NAME — install it by hand."
    # Never report success here: a skipped install used to print "installed"
    # and burned a full test cycle on a build that never reached the console.
    exit 1
fi

if [ "${VERIFY_INSTALL:-0}" != "1" ]; then
    echo "== 5/5 skipped (set VERIFY_INSTALL=1 to read the package back and hash it) =="
    echo "APP_VER $VER installed; the log banner now names the build."
    exit 0
fi
echo "== 5/5 verify the INSTALLED artifact (reads the package back, ~30 s) =="
TMP="$(mktemp -d)"
timeout 300 curl -sS --connect-timeout 10 \
    "ftp://$CONSOLE:2121/user/app/CUSA64004/app.pkg" -o "$TMP/app.pkg" || true
if [[ -s "$TMP/app.pkg" ]]; then
    INSTALLED_SHA="$(sha256sum "$TMP/app.pkg" | cut -d' ' -f1)"
    rm -rf "$TMP"
    [[ "$INSTALLED_SHA" == "$LOCAL_SHA" ]] || {
        echo "FAIL: installed package differs (console $INSTALLED_SHA)"; exit 1; }
    echo "   installed app.pkg == local build"
else
    rm -rf "$TMP"
    echo "   not installed yet (or install pending) — package staged"
fi

echo "== 5/5 done =="
echo "APP_VER $VER installed; start CUSA64004 on the console."
