#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "${SCRIPT_DIR}/.." && pwd)
SFTP_KERNEL_DIR=${SFTP_KERNEL_DIR:-"${ROOT_DIR}/GPU-SFTP/linux-host-kernel"}
MODULES_STAGING_DIR=${MODULES_STAGING_DIR:-"${SFTP_KERNEL_DIR}/modules-staging"}
JOBS=${JOBS:-16}
KERNELRELEASE=${KERNELRELEASE:-}
INSTALL_MODULES=${INSTALL_MODULES:-1}

log() {
	printf '\n==> %s\n' "$*"
}

log "Building Linux-Host-GPU arm64 kernel"
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j"${JOBS}"

if [[ -z "${KERNELRELEASE}" ]]; then
	KERNELRELEASE=$(make -s ARCH=arm64 kernelrelease)
fi

log "Installing host kernel payload to ${SFTP_KERNEL_DIR}"
mkdir -p "${SFTP_KERNEL_DIR}"
cp -v "${SCRIPT_DIR}/arch/arm64/boot/Image" "${SFTP_KERNEL_DIR}/Image"

if [[ "${INSTALL_MODULES}" -eq 1 ]]; then
	log "Installing host modules to staging directory"
	rm -rf "${MODULES_STAGING_DIR}"
	mkdir -p "${MODULES_STAGING_DIR}"
	make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 \
		INSTALL_MOD_PATH="${MODULES_STAGING_DIR}" modules_install
fi

log "Host kernel payload ready"
printf 'Image: %s\n' "${SFTP_KERNEL_DIR}/Image"
if [[ "${INSTALL_MODULES}" -eq 1 ]]; then
	printf 'Modules staging: %s\n' "${MODULES_STAGING_DIR}/lib/modules/${KERNELRELEASE}"
else
	printf 'Modules staging: skipped\n'
fi
