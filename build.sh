#!/bin/bash

set -e  # 遇到错误立即退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 日志函数
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 创建构建目录
BUILD_DIR="$SCRIPT_DIR/build"
BIN_DIR="$SCRIPT_DIR/bin"

log_info "Building RK3588 Multi-Display Manager..."

# 创建构建目录
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

# 进入构建目录
cd "$BUILD_DIR"

# 运行CMake配置
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译项目
make -j$(nproc)

log_success "Build completed successfully"

# 创建bin目录并安装
log_info "Installing RK3588 Multi-Display Manager to bin directory..."

if [ ! -d "$BIN_DIR" ]; then
    mkdir -p "$BIN_DIR"
fi

# 复制可执行文件到bin目录
cp rk3588_multi_display "$BIN_DIR/"

# 复制服务文件模板到bin目录
cp "$SCRIPT_DIR/rk3588-multi-display.service.in" "$BIN_DIR/"

# 创建简单的安装脚本
cat > "$BIN_DIR/install_service.sh" << 'EOF'
#!/bin/bash

# 检查是否以root身份运行
if [ "$EUID" -ne 0 ]; then
    echo "请以root身份运行此脚本来安装systemd服务"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_FILE="/etc/systemd/system/rk3588-multi-display.service"

# 替换服务文件中的路径
sed "s|@EXEC_PATH@|$SCRIPT_DIR/rk3588_multi_display|g" \
    "$SCRIPT_DIR/rk3588-multi-display.service.in" > "$SERVICE_FILE"

# 重新加载systemd
systemctl daemon-reload

echo "Service installed successfully!"
echo "使用以下命令管理服务:"
echo "  启动服务: systemctl start rk3588-multi-display"
echo "  开机自启: systemctl enable rk3588-multi-display"
echo "  查看状态: systemctl status rk3588-multi-display"
echo "  查看日志: journalctl -u rk3588-multi-display -f"
EOF

chmod +x "$BIN_DIR/install_service.sh"

# 创建使用说明
cat > "$BIN_DIR/README.txt" << 'EOF'
RK3588 Multi-Display Manager - 使用说明

文件说明:
- rk3588_multi_display: 主程序
- install_service.sh: systemd服务安装脚本
- rk3588-multi-display.service.in: systemd服务模板

直接运行:
./rk3588_multi_display

安装为系统服务:
sudo ./install_service.sh

命令行选项:
-d, --daemon: 后台运行模式
-v, --verbose: 详细日志输出
-h, --help: 显示帮助信息

注意:
- 需要root权限运行
- 确保DRM设备可访问 (/dev/dri/card0)
- 支持热插拔HDMI和DisplayPort显示器
EOF

log_success "Installation completed!"
log_info "Files installed to: $BIN_DIR"
log_info "Run './bin/rk3588_multi_display' to start the application"
log_info "Run 'sudo ./bin/install_service.sh' to install as system service" 