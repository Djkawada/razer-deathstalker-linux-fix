# Maintainer: Razer DeathStalker Linux Fix Contributors
pkgname=razer-deathstalker-fix-dkms
pkgver=1.0.0
pkgrel=1
pkgdesc="DKMS kernel module fixing the 51.2s USB HID init hang, dead NumLock LED, and hotplug timeout on Razer DeathStalker Essential 2014 (1532:011f)"
arch=('any')
url="https://github.com/Djkawada/razer-deathstalker-linux-fix"
license=('GPL-2.0-only')
depends=('dkms')
source=("razer_deathstalker_fix.c"
        "Makefile"
        "dkms.conf")
sha256sums=('SKIP'
            'SKIP'
            'SKIP')

package() {
    local _src="${pkgdir}/usr/src/razer-deathstalker-fix-${pkgver}"
    install -dm755 "${_src}"
    install -m644 "${srcdir}/razer_deathstalker_fix.c" "${_src}/razer_deathstalker_fix.c"
    install -m644 "${srcdir}/Makefile" "${_src}/Makefile"
    install -m644 "${srcdir}/dkms.conf" "${_src}/dkms.conf"

    install -dm755 "${pkgdir}/usr/lib/modules-load.d"
    echo "razer_deathstalker_fix" > "${pkgdir}/usr/lib/modules-load.d/razer-deathstalker-fix.conf"

    install -dm755 "${pkgdir}/etc/mkinitcpio.conf.d"
    cat << 'EOF' > "${pkgdir}/etc/mkinitcpio.conf.d/razer-deathstalker-fix.conf"
# Early-load razer_deathstalker_fix when built for the target kernel
if [[ $(modinfo -k "${KERNELVERSION:-$(uname -r)}" -F filename razer_deathstalker_fix 2>/dev/null) == /* ]]; then
  MODULES+=(razer_deathstalker_fix)
fi
EOF
}
