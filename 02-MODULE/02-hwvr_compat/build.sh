#!/system/bin/sh
# 在设备上（Termux / KSU 根 shell）或 PC 的 NDK 里编 hwvrctl。
#
#   sh build.sh                 # 直接编到当前目录的 hwvrctl
#   sh build.sh /tmp/hwvrctl    # 指定输出
#
# 设备是 aarch64（OnePlus 7T / sm8150），所以只需要 arm64 目标。
set -e
SRC="$(dirname "$0")/hwvrctl.c"
OUT="${1:-$(dirname "$0")/hwvrctl}"

if [ -n "$CC" ] && command -v "$CC" >/dev/null 2>&1; then
    echo "[build] 用 CC=$CC"
    $CC -O2 -std=gnu99 -o "$OUT" "$SRC" -lm
elif command -v aarch64-linux-android21-clang >/dev/null 2>&1; then
    echo "[build] NDK clang（已在 PATH）"
    aarch64-linux-android21-clang -O2 -std=gnu99 -o "$OUT" "$SRC" -lm
elif [ -n "$ANDROID_NDK_HOME" ] && [ -x "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang" ]; then
    echo "[build] NDK=$ANDROID_NDK_HOME"
    "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang" \
        -O2 -std=gnu99 -o "$OUT" "$SRC" -lm
elif command -v clang >/dev/null 2>&1; then
    # Termux 里 clang 是本地 aarch64 编译器，直接原生编。
    echo "[build] Termux clang（原生 aarch64）"
    clang -O2 -std=gnu99 -o "$OUT" "$SRC" -lm
else
    echo "找不到编译器。二选一：" >&2
    echo "  pkg install clang          # Termux 里" >&2
    echo "  export ANDROID_NDK_HOME=…  # PC 上" >&2
    exit 1
fi

chmod 755 "$OUT"
file "$OUT" 2>/dev/null || true
echo "[build] 完成: $OUT"
