#!/usr/bin/env bash
set -euo pipefail

MODULE_NAME="razer-deathstalker-fix"
MODULE_VERSION="1.0.0"
KO_NAME="razer_deathstalker_fix"
SRC_DIR="/usr/src/${MODULE_NAME}-${MODULE_VERSION}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $EUID -ne 0 ]]; then
    echo "Error: Please run this installer as root (e.g., sudo ./install.sh)" >&2
    exit 1
fi

echo "==> Installing ${MODULE_NAME} v${MODULE_VERSION} source to ${SRC_DIR}..."
mkdir -p "${SRC_DIR}"
install -m 644 "${SCRIPT_DIR}/razer_deathstalker_fix.c" "${SRC_DIR}/razer_deathstalker_fix.c"
install -m 644 "${SCRIPT_DIR}/Makefile" "${SRC_DIR}/Makefile"
install -m 644 "${SCRIPT_DIR}/dkms.conf" "${SRC_DIR}/dkms.conf"

if command -v dkms >/dev/null 2>&1; then
    echo "==> Registering and building module with DKMS..."
    if dkms status -m "${MODULE_NAME}" -v "${MODULE_VERSION}" | grep -q "${MODULE_NAME}"; then
        dkms remove -m "${MODULE_NAME}" -v "${MODULE_VERSION}" --all || true
    fi
    dkms add -m "${MODULE_NAME}" -v "${MODULE_VERSION}"
    dkms build -m "${MODULE_NAME}" -v "${MODULE_VERSION}"
    dkms install -m "${MODULE_NAME}" -v "${MODULE_VERSION}" --force
else
    echo "Warning: dkms not found. Building and installing for running kernel $(uname -r) only..."
    make -C "${SRC_DIR}" clean all
    install -D -m 644 "${SRC_DIR}/${KO_NAME}.ko" "/usr/lib/modules/$(uname -r)/updates/${KO_NAME}.ko"
    depmod -a
fi

echo "==> Configuring automatic load at boot (/etc/modules-load.d/${MODULE_NAME}.conf)..."
mkdir -p /etc/modules-load.d
echo "${KO_NAME}" > "/etc/modules-load.d/${MODULE_NAME}.conf"

echo "==> Configuring early-boot initramfs..."
if [[ -d /etc/mkinitcpio.conf.d ]]; then
    if [[ -f /etc/mkinitcpio.conf.bak ]]; then
        mv -f /etc/mkinitcpio.conf.bak /etc/mkinitcpio.conf
    fi
    cat << 'EOF' > "/etc/mkinitcpio.conf.d/${MODULE_NAME}.conf"
# Early-load razer_deathstalker_fix when built for the target kernel
if [[ $(modinfo -k "${KERNELVERSION:-$(uname -r)}" -F filename razer_deathstalker_fix 2>/dev/null) == /* ]]; then
  MODULES+=(razer_deathstalker_fix)
fi
EOF
elif [[ -f /etc/mkinitcpio.conf ]]; then
    if ! grep -Eq "^MODULES=\(.*\b${KO_NAME}\b.*\)" /etc/mkinitcpio.conf; then
        cp /etc/mkinitcpio.conf /etc/mkinitcpio.conf.bak
        sed -i -E "s/^MODULES=\((.*)\)/MODULES=(${KO_NAME} \1)/; s/MODULES=\(${KO_NAME}  *\)/MODULES=(${KO_NAME})/" /etc/mkinitcpio.conf
    fi
fi

if command -v limine-mkinitcpio >/dev/null 2>&1; then
    limine-mkinitcpio
elif command -v mkinitcpio >/dev/null 2>&1; then
    /usr/bin/mkinitcpio -P
elif command -v dracut >/dev/null 2>&1; then
    mkdir -p /etc/dracut.conf.d
    echo "add_drivers+=\" ${KO_NAME} \"" > "/etc/dracut.conf.d/${MODULE_NAME}.conf"
    dracut --regenerate-all --force
elif command -v update-initramfs >/dev/null 2>&1; then
    if [[ -f /etc/initramfs-tools/modules ]] && ! grep -qx "${KO_NAME}" /etc/initramfs-tools/modules; then
        echo "${KO_NAME}" >> /etc/initramfs-tools/modules
    fi
    update-initramfs -u -k all
fi

echo "==> Loading ${KO_NAME} into the running kernel..."
if lsmod | grep -q "^${KO_NAME}\b"; then
    rmmod "${KO_NAME}" || true
fi
modprobe "${KO_NAME}"

echo "==> Successfully installed and loaded ${KO_NAME}!"
