#!/usr/bin/env bash
set -euo pipefail

MODULE_NAME="razer-deathstalker-fix"
MODULE_VERSION="1.0.0"
KO_NAME="razer_deathstalker_fix"
SRC_DIR="/usr/src/${MODULE_NAME}-${MODULE_VERSION}"

if [[ $EUID -ne 0 ]]; then
    echo "Error: Please run this uninstaller as root (e.g., sudo ./uninstall.sh)" >&2
    exit 1
fi

echo "==> Unloading ${KO_NAME} from running kernel..."
if [[ -d "/sys/module/${KO_NAME}" ]]; then
    rmmod "${KO_NAME}" || true
fi

if command -v dkms >/dev/null 2>&1; then
    if dkms status -m "${MODULE_NAME}" -v "${MODULE_VERSION}" | grep -q "${MODULE_NAME}"; then
        echo "==> Removing ${MODULE_NAME} from DKMS..."
        dkms remove -m "${MODULE_NAME}" -v "${MODULE_VERSION}" --all || true
    fi
fi

rm -rf "${SRC_DIR}"
rm -f "/etc/modules-load.d/${MODULE_NAME}.conf"
rm -f "/etc/mkinitcpio.conf.d/${MODULE_NAME}.conf"
rm -f "/etc/limine-entry-tool.d/${MODULE_NAME}.conf"
if [[ -f "/etc/default/grub.d/${MODULE_NAME}.cfg" ]]; then
    rm -f "/etc/default/grub.d/${MODULE_NAME}.cfg"
    if command -v update-grub >/dev/null 2>&1; then
        update-grub
    fi
fi

echo "==> Updating initramfs..."
if [[ -f /etc/mkinitcpio.conf ]]; then
    sed -i -E "s/\b${KO_NAME}\b *//g" /etc/mkinitcpio.conf
fi

if command -v limine-mkinitcpio >/dev/null 2>&1; then
    limine-mkinitcpio
elif command -v mkinitcpio >/dev/null 2>&1; then
    /usr/bin/mkinitcpio -P
elif command -v dracut >/dev/null 2>&1; then
    rm -f "/etc/dracut.conf.d/${MODULE_NAME}.conf"
    dracut --regenerate-all --force
elif command -v update-initramfs >/dev/null 2>&1; then
    if [[ -f /etc/initramfs-tools/modules ]]; then
        sed -i "/^${KO_NAME}$/d" /etc/initramfs-tools/modules
    fi
    update-initramfs -u -k all
fi

echo "==> Successfully uninstalled ${KO_NAME}."
