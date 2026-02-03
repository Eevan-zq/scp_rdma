#!/bin/bash
#
# scp_rdma 安装/卸载脚本
# 使用方法:
#   ./install.sh          - 安装到 /usr/local/bin
#   ./install.sh uninstall - 卸载
#   ./install.sh --prefix=/custom/path - 安装到自定义路径
#

set -e

# 默认安装路径
PREFIX="/usr/local"
BUILD_DIR="build"
BINARY_NAME="scp_rdma"

# 解析参数
ACTION="install"
for arg in "$@"; do
    case "$arg" in
        uninstall)
            ACTION="uninstall"
            ;;
        --prefix=*)
            PREFIX="${arg#*=}"
            ;;
        --help|-h)
            echo "Usage: $0 [install|uninstall] [--prefix=PATH]"
            echo ""
            echo "Commands:"
            echo "  install     Install scp_rdma to system (default)"
            echo "  uninstall   Remove scp_rdma from system"
            echo ""
            echo "Options:"
            echo "  --prefix=PATH  Install to custom path (default: /usr/local)"
            exit 0
            ;;
    esac
done

BIN_DIR="${PREFIX}/bin"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
    exit 1
}

do_build() {
    info "Building scp_rdma..."
    
    # 如果 build 目录不存在，创建并配置
    if [ ! -d "$BUILD_DIR" ]; then
        meson setup "$BUILD_DIR" --prefix="$PREFIX"
    else
        # 更新 prefix
        meson configure "$BUILD_DIR" --prefix="$PREFIX"
    fi
    
    # 编译
    ninja -C "$BUILD_DIR"
}

do_install() {
    info "Installing scp_rdma to ${BIN_DIR}..."
    
    # 先编译
    do_build
    
    # 使用 meson install (需要 sudo 如果安装到系统路径)
    if [[ "$PREFIX" == "/usr"* ]]; then
        info "Installing to system path, sudo required..."
        sudo ninja -C "$BUILD_DIR" install
    else
        ninja -C "$BUILD_DIR" install
    fi
    
    info "Installation complete!"
    info "You can now run 'scp_rdma' from anywhere."
    
    # 验证安装
    if command -v scp_rdma &> /dev/null; then
        info "Verified: $(which scp_rdma)"
    else
        warn "Binary installed to ${BIN_DIR}/scp_rdma"
        warn "Make sure ${BIN_DIR} is in your PATH"
    fi
}

do_uninstall() {
    info "Uninstalling scp_rdma..."
    
    INSTALLED_PATH="${BIN_DIR}/${BINARY_NAME}"
    
    if [ -f "$INSTALLED_PATH" ]; then
        if [[ "$PREFIX" == "/usr"* ]]; then
            info "Removing from system path, sudo required..."
            sudo rm -f "$INSTALLED_PATH"
        else
            rm -f "$INSTALLED_PATH"
        fi
        info "Removed: $INSTALLED_PATH"
    else
        warn "scp_rdma not found at $INSTALLED_PATH"
    fi
    
    # 也检查通过 which 找到的路径
    WHICH_PATH=$(which scp_rdma 2>/dev/null || true)
    if [ -n "$WHICH_PATH" ] && [ "$WHICH_PATH" != "$INSTALLED_PATH" ]; then
        warn "Another installation found at: $WHICH_PATH"
        read -p "Remove it too? [y/N] " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            sudo rm -f "$WHICH_PATH"
            info "Removed: $WHICH_PATH"
        fi
    fi
    
    info "Uninstall complete!"
}

# 主逻辑
case "$ACTION" in
    install)
        do_install
        ;;
    uninstall)
        do_uninstall
        ;;
    *)
        error "Unknown action: $ACTION"
        ;;
esac
