#!/bin/sh
# 把 vo_rockit(一层多通道双摄显示) 运行所需的 rockit 依赖库推送到板子。
# 库文件来源见 deps/README.md。
#
# 用法:
#   ./install_deps.sh                 # 默认设备 192.168.3.90
#   ./install_deps.sh <adb-serial>    # 指定 adb 设备号
#
# 说明:
#   - 板上 adbd 以 root 运行, adb push 可直接写入 /usr/lib
#   - 只新增文件, 不覆盖/替换系统已有库(Mesa 的 libEGL 等不受影响)
#   - libmali 仅被 rockit 的 GPU composer(libgraphic_lsf.so)按 NEEDED 加载,
#     与 Xorg 使用的 Mesa 库互不干扰

set -e

SERIAL=${1:-192.168.3.90}
LIB_DIR=$(cd "$(dirname "$0")"; pwd)/deps/lib
TARGET=/usr/lib/aarch64-linux-gnu

ADB="adb -s $SERIAL"

echo "== 推送依赖库到 $SERIAL:$TARGET =="
for lib in librockit.so libgraphic_lsf.so libmali.so.1.9.0 libmali-hook.so.1.9.0; do
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
	chmod 644 librockit.so libgraphic_lsf.so libmali.so.1.9.0 libmali-hook.so.1.9.0 && \
	ldconfig && \
	ldconfig -p | grep -E 'rockit|graphic_lsf|libmali'"

echo "== 完成 =="
