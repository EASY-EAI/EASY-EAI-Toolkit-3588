#!/bin/sh
# 推送 GPU 运行依赖（libmali / libgraphic_lsf）。
# 库文件说明见 deps/README.md。
#
# 用法:
#   ./install_deps.sh                 # 默认设备 192.168.3.90:5555（网络 adb，自动 connect）
#   ./install_deps.sh 192.168.3.143   # 按 IP 连接（默认端口 5555）
#   ./install_deps.sh 192.168.3.143:5555
#   ./install_deps.sh ca967bfa2567e6bf  # USB 设备号直接透传

set -e

SERIAL=${1:-192.168.3.90}
LIB_DIR=$(cd "$(dirname "$0")"; pwd)/deps/lib
TARGET=/usr/lib/aarch64-linux-gnu

# 含 "." 视为 IP/主机名：缺端口补 5555，并先建立网络 adb 连接；
# USB 设备号（无 "."）直接使用。
case "$SERIAL" in
    *.*)
        case "$SERIAL" in
            *:*) ;;
            *) SERIAL="$SERIAL:5555" ;;
        esac
        echo "== adb connect $SERIAL =="
        adb connect "$SERIAL" || true
        ;;
esac

ADB="adb -s $SERIAL"

echo "== 推送 GPU 可选依赖到 $SERIAL:$TARGET =="
for lib in libgraphic_lsf.so libmali.so.1.9.0 libmali-hook.so.1.9.0; do
	if [ ! -f "$LIB_DIR/$lib" ]; then
		echo "错误: 缺少 $LIB_DIR/$lib (来源见 deps/README.md)" >&2
		exit 1
	fi
	$ADB push "$LIB_DIR/$lib" "$TARGET/$lib"
done

echo "== 建 libmali 符号链接 + 刷新链接器缓存 =="
$ADB shell "cd $TARGET && \
	ln -sf libmali.so.1.9.0 libmali.so.1 && \
	ln -sf libmali.so.1 libmali.so && \
	ln -sf libmali-hook.so.1.9.0 libmali-hook.so.1 && \
	chmod 644 libgraphic_lsf.so libmali.so.1.9.0 libmali-hook.so.1.9.0 && \
	ldconfig && \
	ldconfig -p | grep -E 'graphic_lsf|libmali'"

echo "== 完成 =="
