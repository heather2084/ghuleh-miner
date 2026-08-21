#!/data/data/com.termux/files/usr/bin/bash
#
# Build the Ghuleh Miner APK *on an Android phone in Termux* (aarch64-native),
# bypassing Gradle/AGP entirely with the raw aapt2 -> kotlinc -> d8 -> apksigner
# pipeline. AGP in Termux is painful (it pulls x86 aapt2/d8 + wants a full SDK);
# this uses Termux's native tools directly.
#
# One-time deps:
#   pkg install openjdk-17 kotlin aapt aapt2 d8 apksigner android-tools zip
#
# Inputs you must stage first:
#   1. Build the SELF-CONTAINED native miner with android/build_native_termux.sh
#      — it stages app/src/main/jniLibs/arm64-v8a/{libprimo.so,libc++_shared.so}
#      itself (static curl/mbedTLS/jansson, bionic-only NEEDED deps). Do NOT
#      package the plain build_termux.sh binary: it links Termux's shared
#      libcurl/libjansson, which don't exist inside the app sandbox, so the
#      miner won't start (the preflight below rejects it when readelf exists).
#   2. android.jar (API 33) — NOT a Termux package; fetched automatically below
#      from a public platforms mirror into ~/.primo-android-sdk/ (override with
#      ANDROID_JAR=/path/to/android.jar).
#
# Output: build/ghuleh-miner.apk (debug-signed; install with `adb install` or tap).
set -euo pipefail

API=33
PKG=dev.ghuleh.miner
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/app/src/main"
OUT="$HERE/build"
GEN="$OUT/gen"
CLASSES="$OUT/classes"
SDK_CACHE="${SDK_CACHE:-$HOME/.primo-android-sdk}"
ANDROID_JAR="${ANDROID_JAR:-$SDK_CACHE/android-$API/android.jar}"
JNILIB="$APP/jniLibs/arm64-v8a/libprimo.so"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"

say() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# --- preflight ---------------------------------------------------------------
for t in aapt2 d8 apksigner zipalign kotlinc keytool javac zip; do
  command -v "$t" >/dev/null 2>&1 || die "missing '$t' — run: pkg install openjdk-17 kotlin aapt aapt2 d8 apksigner android-tools zip"
done
[ -f "$JNILIB" ] || die "native miner not staged at $JNILIB (see header step 1)"

# The packaged miner must be self-contained: every DT_NEEDED entry must be
# either bionic or staged alongside it in jniLibs. A Termux-linked binary
# (plain build_termux.sh output) passes the exists-check above but cannot
# start inside the app sandbox — catch it here instead of on-device.
if command -v readelf >/dev/null 2>&1; then
  MISSING_NEEDED=""
  while read -r so; do
    case "$so" in
      libc.so|libm.so|libdl.so|liblog.so) ;;                # bionic
      *) [ -f "$(dirname "$JNILIB")/$so" ] || MISSING_NEEDED="$MISSING_NEEDED $so" ;;
    esac
  done < <(readelf -d "$JNILIB" | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p')
  [ -z "$MISSING_NEEDED" ] || die "libprimo.so NEEDED deps not bionic/staged:$MISSING_NEEDED — build it with android/build_native_termux.sh (see header step 1)"
else
  say "readelf not found — skipping libprimo.so self-containedness check (pkg install binutils)"
fi

# kotlin-stdlib must be on the dex; locate the Termux copy.
KOTLIN_STDLIB=$(find "$PREFIX" -name 'kotlin-stdlib.jar' 2>/dev/null | head -1)
[ -n "$KOTLIN_STDLIB" ] || die "kotlin-stdlib.jar not found under \$PREFIX (pkg install kotlin)"

# --- android.jar (platform stub) --------------------------------------------
if [ ! -f "$ANDROID_JAR" ]; then
  say "fetching android.jar API $API (one-time)…"
  mkdir -p "$(dirname "$ANDROID_JAR")"
  URL="https://raw.githubusercontent.com/Sable/android-platforms/master/android-$API/android.jar"
  curl -fSL "$URL" -o "$ANDROID_JAR" || die "could not download android.jar — set ANDROID_JAR=/path/to/android.jar"
fi

# --- clean -------------------------------------------------------------------
rm -rf "$OUT"
mkdir -p "$GEN" "$CLASSES" "$OUT/apk"

# --- 1. resources: compile + link (emits R.java + a resources-only APK) ------
say "aapt2: compiling resources"
aapt2 compile --dir "$APP/res" -o "$OUT/res.zip"

say "aapt2: linking"
aapt2 link \
  -I "$ANDROID_JAR" \
  --manifest "$APP/AndroidManifest.xml" \
  --java "$GEN" \
  --min-sdk-version 24 \
  --target-sdk-version "$API" \
  -o "$OUT/base.apk" \
  "$OUT/res.zip"

# --- 2. compile R.java (javac) + Kotlin sources (kotlinc) --------------------
# NB: Termux's default JDK is 21 and d8 (build-tools 33) only fully supports
# Java 11 bytecode (class 55), so pin both compilers to 11.
say "javac: R.java"
javac --release 11 -d "$CLASSES" -classpath "$ANDROID_JAR" $(find "$GEN" -name '*.java')

say "kotlinc: app sources"
kotlinc \
  -classpath "$ANDROID_JAR:$CLASSES" \
  -d "$CLASSES" \
  -jvm-target 11 \
  $(find "$APP/kotlin" -name '*.kt')

# --- 3. dex (d8) -------------------------------------------------------------
say "d8: classes -> dex"
d8 --min-api 24 --lib "$ANDROID_JAR" --output "$OUT/apk" \
  "$KOTLIN_STDLIB" \
  $(find "$CLASSES" -name '*.class')

# --- 4. assemble APK: base resources + dex + native libs ---------------------
say "assembling apk"
cp "$OUT/base.apk" "$OUT/unsigned.apk"
( cd "$OUT/apk" && zip -q "$OUT/unsigned.apk" classes.dex )
# Bundle EVERY lib*.so staged in jniLibs/arm64-v8a (libprimo.so + its remaining
# dynamic deps, e.g. libc++.so). Must be named lib*.so to be APK-packageable.
JNILIB_DIR="$APP/jniLibs/arm64-v8a"
mkdir -p "$OUT/lib/arm64-v8a"
cp "$JNILIB_DIR"/*.so "$OUT/lib/arm64-v8a/"
( cd "$OUT" && zip -q "$OUT/unsigned.apk" lib/arm64-v8a/*.so )

# License texts ride inside the APK (assets/licenses/) — the APK is a binary
# distribution of the GPL-3.0 miner + BSD-3 RandomX + Apache-2.0-derived
# CLHash, so the texts must accompany it (see NOTICE). The first-launch
# disclaimer points here.
say "bundling license texts"
ROOT="$HERE/.."
mkdir -p "$OUT/assets/licenses"
cp "$ROOT/LICENSE"                        "$OUT/assets/licenses/LICENSE-GPL-3.0.txt"
cp "$ROOT/NOTICE"                         "$OUT/assets/licenses/NOTICE.txt"
cp "$ROOT/third_party/RandomX/LICENSE"    "$OUT/assets/licenses/BSD-3-Clause-RandomX.txt"
cp "$ROOT/LICENSES/Apache-2.0.txt"        "$OUT/assets/licenses/Apache-2.0.txt"
( cd "$OUT" && zip -q "$OUT/unsigned.apk" assets/licenses/* )

# --- 5. align + sign ---------------------------------------------------------
# Release signing: set PRIMO_KEYSTORE (path to the RELEASE .jks) and
# PRIMO_KS_PASS (store+key password); optional PRIMO_KS_ALIAS (default
# "ghuleh"). The release key is the app's identity — updates only install
# over an APK signed with the SAME key. Never regenerate it; keep off-device
# backups. Unset = debug keystore, auto-generated, exactly as before.
if [ -n "${PRIMO_KEYSTORE:-}" ]; then
  [ -f "$PRIMO_KEYSTORE" ] || die "PRIMO_KEYSTORE not found: $PRIMO_KEYSTORE"
  [ -n "${PRIMO_KS_PASS:-}" ] || die "PRIMO_KS_PASS not set (release keystore password)"
  KEYSTORE="$PRIMO_KEYSTORE"
  KS_PASS="$PRIMO_KS_PASS"
  KS_ALIAS="${PRIMO_KS_ALIAS:-ghuleh}"
  SIGNED_NAME="ghuleh-miner-release.apk"
  say "RELEASE signing with $KEYSTORE (alias $KS_ALIAS)"
else
  KEYSTORE="$SDK_CACHE/debug.keystore"
  KS_PASS="android"
  KS_ALIAS="androiddebugkey"
  SIGNED_NAME="ghuleh-miner.apk"
  if [ ! -f "$KEYSTORE" ]; then
    say "generating debug keystore"
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
      -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
      -dname "CN=Ghuleh Debug,O=Ghuleh Miner,C=US"
  fi
fi

say "zipalign"
zipalign -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

say "apksigner"
# env: form keeps the password out of the process command line (visible to
# every local process via /proc/*/cmdline with pass: form).
export KS_PASS
apksigner sign \
  --ks "$KEYSTORE" --ks-pass env:KS_PASS --key-pass env:KS_PASS \
  --ks-key-alias "$KS_ALIAS" \
  --out "$OUT/$SIGNED_NAME" "$OUT/aligned.apk"

say "done -> $OUT/$SIGNED_NAME"
apksigner verify --print-certs "$OUT/$SIGNED_NAME" | grep -i 'SHA-256 digest' || true
apksigner verify "$OUT/$SIGNED_NAME" >/dev/null && say "signature OK"
