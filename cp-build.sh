#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "${SCRIPT_DIR}/.." && pwd)
SFTP_KERNEL_DIR=${SFTP_KERNEL_DIR:-"${ROOT_DIR}/GPU-SFTP/linux-host-kernel"}
MODULES_STAGING_DIR=${MODULES_STAGING_DIR:-"${SFTP_KERNEL_DIR}/modules-staging"}
JOBS=${JOBS:-16}
KERNELRELEASE=${KERNELRELEASE:-}
INSTALL_MODULES=${INSTALL_MODULES:-0}
HOST_MODULES=${HOST_MODULES:-}

log() {
	printf '\n==> %s\n' "$*"
}

if [[ "${INSTALL_MODULES}" -eq 1 && -z "${HOST_MODULES}" ]]; then
	log "Building Linux-Host-GPU arm64 kernel and modules"
	make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j"${JOBS}"
elif [[ "${INSTALL_MODULES}" -eq 1 ]]; then
	read -r -a module_targets <<<"${HOST_MODULES}"
	log "Building Linux-Host-GPU arm64 Image and selected modules: ${HOST_MODULES}"
	make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j"${JOBS}" \
		KBUILD_MODPOST_WARN=1 Image "${module_targets[@]}"
else
	log "Building Linux-Host-GPU arm64 Image only"
	make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j"${JOBS}" Image
fi

if [[ -z "${KERNELRELEASE}" ]]; then
	KERNELRELEASE=$(make -s ARCH=arm64 kernelrelease)
fi

log "Installing host kernel payload to ${SFTP_KERNEL_DIR}"
mkdir -p "${SFTP_KERNEL_DIR}"
cp -v "${SCRIPT_DIR}/arch/arm64/boot/Image" "${SFTP_KERNEL_DIR}/Image"

if [[ "${INSTALL_MODULES}" -eq 1 && -z "${HOST_MODULES}" ]]; then
	log "Installing host modules to staging directory"
	rm -rf "${MODULES_STAGING_DIR}"
	mkdir -p "${MODULES_STAGING_DIR}"
	make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 \
		INSTALL_MOD_PATH="${MODULES_STAGING_DIR}" modules_install
elif [[ "${INSTALL_MODULES}" -eq 1 ]]; then
	read -r -a selected_modules <<<"${HOST_MODULES}"
	log "Installing selected host modules to staging directory"
	rm -rf "${MODULES_STAGING_DIR}"
	mkdir -p "${MODULES_STAGING_DIR}/lib/modules/${KERNELRELEASE}/kernel"
	: >"${MODULES_STAGING_DIR}/selected-modules.txt"
	for module in "${selected_modules[@]}"; do
		module=${module#./}
		[[ "${module}" == *.ko ]] || module="${module}.ko"
		[[ -f "${module}" ]] || {
			echo "missing selected module: ${module}" >&2
			exit 1
		}
		install -D -m 0644 "${module}" \
			"${MODULES_STAGING_DIR}/lib/modules/${KERNELRELEASE}/kernel/${module}"
		printf '%s\n' "${module}" >>"${MODULES_STAGING_DIR}/selected-modules.txt"
	done
fi

log "Host kernel payload ready"
printf 'Image: %s\n' "${SFTP_KERNEL_DIR}/Image"
if [[ "${INSTALL_MODULES}" -eq 1 ]]; then
	if [[ -n "${HOST_MODULES}" ]]; then
		printf 'Modules staging: %s\n' "${MODULES_STAGING_DIR}/lib/modules/${KERNELRELEASE}"
		printf 'Selected modules: %s\n' "${HOST_MODULES}"
	else
		printf 'Modules staging: %s\n' "${MODULES_STAGING_DIR}/lib/modules/${KERNELRELEASE}"
	fi
else
	printf 'Modules staging: skipped\n'
fi
