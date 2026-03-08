#!/bin/bash
set -e
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="${ROOT_DIR}/build"
INSTALL_DIR="${BUILD_DIR}/install"
OUTPUT_DIR="${ROOT_DIR}/output"
RPM_SPEC="${ROOT_DIR}/rpm/os-transport.spec"
PKG_NAME="os-transport"
PKG_VERSION="1.0.0"
PKG_RELEASE="1"

# 步骤1：检查依赖（cmake/gcc/rpmbuild）
echo -e "${YELLOW}[1/6] 检查编译/打包依赖...${NC}"
deps=("cmake" "gcc" "rpmbuild" "make")
for dep in "${deps[@]}"; do
    if ! command -v "${dep}" &> /dev/null; then
        echo -e "${RED}错误：未安装 ${dep}，请执行：sudo apt install ${dep} (Ubuntu) 或 yum install ${dep} (CentOS)${NC}"
        exit 1
    fi
done

# 步骤2：清理旧文件
echo -e "${YELLOW}[2/6] 清理旧编译/打包文件...${NC}"
rm -rf "${BUILD_DIR}" "${OUTPUT_DIR}"
mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}" "${OUTPUT_DIR}"

# 步骤3：CMake配置（指定安装路径）
echo -e "${YELLOW}[3/6] 执行CMake配置...${NC}"
cd "${BUILD_DIR}"
cmake \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-Wall -Wextra -O2 -fPIC" \
    ..

# 步骤4：编译生成libos_transport.so
echo -e "${YELLOW}[4/6] 编译生成共享库...${NC}"
make -j$(nproc 2>/dev/null || echo 4)

# 步骤5：安装到临时目录（供RPM打包）
echo -e "${YELLOW}[5/6] 临时安装到 ${INSTALL_DIR}...${NC}"
make install DESTDIR="${INSTALL_DIR}"

# 步骤6：构建RPM包并输出到output目录
echo -e "${YELLOW}[6/6] 打包生成RPM文件...${NC}"
rpmbuild -bb \
    --define "_topdir ${BUILD_DIR}/rpmbuild" \
    --define "_builddir ${ROOT_DIR}" \
    --define "_rpmdir ${OUTPUT_DIR}" \
    --define "_srcrpmdir ${OUTPUT_DIR}" \
    --define "version ${PKG_VERSION}" \
    --define "release ${PKG_RELEASE}" \
    --define "install_root ${INSTALL_DIR}" \
    "${RPM_SPEC}"

# 验证结果
RPM_FILE=$(find "${OUTPUT_DIR}" -name "${PKG_NAME}-${PKG_VERSION}-${PKG_RELEASE}*.rpm" | head -1)
if [ -f "${RPM_FILE}" ]; then
    echo -e "${GREEN}=======================================${NC}"
    echo -e "${GREEN}✅ 编译+打包成功！${NC}"
    echo -e "${GREEN}📦 RPM包路径：${RPM_FILE}${NC}"
    echo -e "${GREEN}🔍 RPM包信息：${NC}"
    rpm -qlp "${RPM_FILE}"  # 显示RPM包含的文件
else
    echo -e "${RED}❌ 打包失败：未找到RPM文件${NC}"
    exit 1
fi