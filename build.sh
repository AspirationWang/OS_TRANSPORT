#!/bin/bash
set -e  # 遇到错误立即退出

# 定义颜色（方便输出提示）
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 仓库根目录（脚本所在目录）
ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="${ROOT_DIR}/build"
TARGET_LIB="${BUILD_DIR}/libos_transport.so"

# 步骤1：检查依赖（cmake、gcc）
echo -e "${YELLOW}[1/4] 检查编译依赖...${NC}"
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}错误：未安装cmake，请先执行：sudo apt install cmake (Ubuntu) 或 brew install cmake (macOS)${NC}"
    exit 1
fi

if ! command -v gcc &> /dev/null; then
    echo -e "${RED}错误：未安装gcc，请先执行：sudo apt install gcc (Ubuntu) 或 xcode-select --install (macOS)${NC}"
    exit 1
fi

# 步骤2：清理旧编译文件（可选，确保编译干净）
echo -e "${YELLOW}[2/4] 清理旧编译文件...${NC}"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

# 步骤3：执行cmake生成编译文件
echo -e "${YELLOW}[3/4] 执行CMake配置...${NC}"
cd "${BUILD_DIR}"
cmake ..

# 步骤4：编译生成共享库
echo -e "${YELLOW}[4/4] 编译生成libos_transport.so...${NC}"
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)  # 自动多核编译

# 验证编译结果
if [ -f "${TARGET_LIB}" ]; then
    echo -e "${GREEN}编译成功！生成的共享库路径：${TARGET_LIB}${NC}"
    # 可选：显示库信息
    echo -e "${YELLOW}库信息：${NC}"
    ls -lh "${TARGET_LIB}"
    file "${TARGET_LIB}"
else
    echo -e "${RED}编译失败：未找到 ${TARGET_LIB}${NC}"
    exit 1
fi