# Razer DeathStalker Essential 2014 (`1532:011f`) — Linux USB HID Fix

Fixes the **51.2-second USB initialization hang (`-110` / `ETIMEDOUT`)**, **dead NumLock / CapsLock / ScrollLock LEDs**, and **unplug/replug failure** on the **Razer DeathStalker Essential 2014** (`USB ID 1532:011f`) across Linux kernels.

| Metric | Before Fix | After Fix |
| :--- | :--- | :--- |
| **USB Initialization Time** | `~51.2 seconds` (`usbhid: can't add hid device: -110`) | **`< 0.2 seconds`** |
| **Hotplug (Unplug / Replug)** | Fails ~75% of the time (`-110` timeout, dead keyboard) | **100% reliable** |
| **NumLock / Status LEDs** | Dead (`usb_submit_urb(ctrl) failed: -1`) | **Working immediately** |
| **Active Interfaces** | Only `1.0` partially binds after 51s timeout | **All 3 interfaces (`1.0`, `1.1`, `1.2`) active** |

---

## Symptoms

When plugging in or booting with a **Razer DeathStalker Essential 2014** (`1532:011f`), `dmesg` / `journalctl -k` shows:

```text
usb 3-6.2: new full-speed USB device number 5 using xhci_hcd
usb 3-6.2: New USB device found, idVendor=1532, idProduct=011f, bcdDevice= 2.00
usb 3-6.2: Product: Razer DeathStalker Essential 2014
usb 3-6.2: Manufacturer: Razer
input: Razer Razer DeathStalker Essential 2014 as /devices/.../input/input5
hid-generic 0003:1532:011F.0001: input,hidraw0: USB HID v1.11 Keyboard [Razer Razer DeathStalker Essential 2014] on usb-.../input0
usbhid 3-6.2:1.0: usb_submit_urb(ctrl) failed: -1
usbhid 3-6.2:1.1: can't add hid device: -110
usbhid 3-6.2:1.1: probe with driver usbhid failed with error -110
usbhid 3-6.2:1.2: can't add hid device: -110
usbhid 3-6.2:1.2: probe with driver usbhid failed with error -110
```

During those **51.2 seconds** (`2 × 25.6s` USB control transfer timeouts on interfaces `1.1` and `1.2`), the USB enumeration thread (`kworker` / `usb_hub_wq`) is blocked, the NumLock LED never turns on, and unplugging/replugging the keyboard often leaves it completely unresponsive.

---

## Root Cause Analysis

The Razer DeathStalker Essential 2014 exposes **3 USB HID interfaces** sharing a single USB Control Endpoint 0 (`EP0`, `bMaxPacketSize0 = 64`):

1. **Interface `0` (`1.0`)**: Boot Keyboard (`61`-byte HID report descriptor, `ep_81` IN, plus status LEDs controlled via `SET_REPORT` on `EP0`).
2. **Interface `1` (`1.1`)**: NKRO Keyboard + Consumer Control + System Control (`179`-byte HID report descriptor, `ep_82` IN).
3. **Interface `2` (`1.2`)**: Virtual Mouse + 90-byte Vendor Feature Report (`94`-byte HID report descriptor, `ep_83` IN).

Three interacting firmware/kernel behaviors wedge the keyboard microcontroller's `EP0` state machine on Linux:

### 1. `usbhid_init_reports()` (`HID_REQ_GET_REPORT`) Destabilizes `EP0`
By default, unless `HID_QUIRK_NO_INIT_REPORTS` is set, `usbhid_start()` calls `usbhid_init_reports()`, which queries every feature/input report over `EP0` using `HID_REQ_GET_REPORT` (`0x01`). Like several other Razer keyboards in `drivers/hid/hid-quirks.c`, the DeathStalker Essential 2014 MCU does not handle `GET_REPORT` initialization probes cleanly.

### 2. Dirty `EP0` State After Class `GET_DESCRIPTOR(HID_DT_REPORT)` (`bmRequestType = 0x81`)
When `usbhid_parse()` downloads the `61`-byte `HID_DT_REPORT` descriptor on Interface `0` (`bmRequestType = 0x81`, `bRequest = 0x06`, `wValue = 0x2200`), the keyboard's microcontroller leaves its `EP0` parser in a state where an immediate class-specific `HID_REQ_SET_REPORT` (`bmRequestType = 0x21`, `bRequest = 0x09` — used to light the NumLock LED) or `HID_REQ_SET_IDLE` either fails or locks up `EP0`.
- Issuing a standard 18-byte `GET_DESCRIPTOR(USB_DT_DEVICE)` (`bmRequestType = 0x80`, `bRequest = 0x06`, `wValue = 0x0100`) on `EP0` **immediately before and immediately after** `usbhid_parse()` resets the MCU's `EP0` state machine cleanly.

### 3. Asynchronous `led_work` (`SET_REPORT`) Race vs. Interface `1.1` Enumeration
When Interface `0` (`1.0`) finishes registering its input device (`hidinput_connect()` -> `input_register_device()` -> `input_leds_connect()`), the Linux kernel schedules `hdev->led_work` (`hidinput_led_worker`) asynchronously on `system_wq` to set the initial NumLock/CapsLock LED state via `EP0`.
- At that **exact same microsecond**, `usb_set_configuration()` moves on to probe Interface `1` (`1.1`) on `EP0`.
- Furthermore, on Interface `1`'s first `179`-byte (`0xb3`) `GET_DESCRIPTOR(HID_DT_REPORT)` request, the MCU occasionally drops one 64-byte packet and returns `115` bytes (`179 - 64`). When `usbhid_parse()` immediately retries without resetting `EP0`, the MCU stops responding on `EP0` entirely, causing `1.1` and `1.2` to time out for `25.6s + 25.6s = 51.2s` (`-110`).

### 4. Competing LED Output Reports & Built-in `CONFIG_USB_HID=y` Pre-Initramfs Probe
- **Competing LED Output Reports:** Both Interface `1.0` (61 bytes) and Interface `1.1` (179 bytes) declare LED Output blocks. Because Interface `1.1` is the active NKRO keyboard where all user keystrokes originate in Linux, Wayland/Hyprland sends LED updates strictly to Interface `1.1`. If both declare LEDs, Linux sends simultaneous `HID_REQ_SET_REPORT` control transfers to both `wIndex = 0` and `wIndex = 1` on `EP0`, wedging the MCU (`-110`). By masking the unused LED descriptor on Interface `1.0`, Interface `1.1` exclusively and reliably drives the physical LEDs.
- **Pre-Initramfs Built-in `usbhid` Probe:** On kernels with `CONFIG_USB_HID=y` built into `vmlinuz`, USB enumeration and `usbhid` probing occur ~1.4s into boot inside `wait_for_device_probe()`, **before** initramfs `/init` runs (~52.9s later after `1.1` and `1.2` time out).

---

## How This Module Fixes It

`razer_deathstalker_fix` is a lightweight, zero-overhead Linux kernel module that registers bus notifiers on `usb_bus_type` and `hid_bus_type` matching **only** `1532:011f`, plus a cold-boot handoff:

1. **Cold-Boot Pre-Initramfs Protection (`usbhid.quirks=0x1532:0x011f:0x00000004`):**
   Passing `HID_QUIRK_IGNORE` (`0x04`) on the kernel command line tells built-in `usbhid` to skip `1532:011f` during the pre-initramfs kernel boot phase with zero `EP0` traffic. When `razer_deathstalker_fix` loads at the start of initramfs, it calls `hid_quirks_exit(BUS_USB)` to clear the ignore quirk and immediately adopts all `1532:011f` interfaces (`1.0`, `1.1`, `1.2`) with full `EP0` synchronization.
2. **Before `1.0` `usbhid_parse()` (`usb_bus_type` `BUS_NOTIFY_ADD_DEVICE`):**
   Sends an 18-byte `GET_DESCRIPTOR(USB_DT_DEVICE)` on `EP0` to ensure a clean `EP0` state after `SET_CONFIGURATION(1)`.
3. **After `1.0` `usbhid_parse()` & Before `usbhid_start()` (`hid_bus_type` `BUS_NOTIFY_ADD_DEVICE`):**
   Applies `HID_QUIRK_NO_INIT_REPORTS | HID_QUIRK_NOGET` (`0x20000008`) to `hdev->quirks`, masks the unused LED Output descriptor on Interface `1.0` (`dev_rdesc[0x37] = 0x01` `Const`), and sends an 18-byte `GET_DESCRIPTOR(USB_DT_DEVICE)` on `EP0`. Masking Interface `1.0`'s LEDs ensures that Interface `1.1` (the primary NKRO keyboard generating keystrokes) owns the LEDs so Wayland/Hyprland modifiers toggle hardware LEDs in real-time without competing `SET_REPORT` control transfers on EP0.
4. **Before `1.1` and `1.2` Probe (`usb_bus_type` `BUS_NOTIFY_ADD_DEVICE`):**
   Primes `1.1`'s `179`-byte report descriptor, flushes any in-flight LED work, and resets `EP0` with `GET_DESCRIPTOR(USB_DT_DEVICE)` before `usbhid_parse()` runs on `1.1` and `1.2`.

### Kernel Log With Fix Installed

```text
usb 3-6.2: new full-speed USB device number 16 using xhci_hcd
usb 3-6.2: New USB device found, idVendor=1532, idProduct=011f, bcdDevice= 2.00
usb 3-6.2: Product: Razer DeathStalker Essential 2014
usb 3-6.2: Manufacturer: Razer
usb 3-6.2:1.0: razer_deathstalker_fix: synchronized EP0 for primary keyboard interface 1.0
hid-generic 0003:1532:011F.0014: razer_deathstalker_fix: applied NO_INIT_REPORTS | NOGET (quirks=0x20000008)
input: Razer Razer DeathStalker Essential 2014 as /devices/.../input/input28
hid-generic 0003:1532:011F.0014: input,hidraw0: USB HID v1.11 Keyboard [Razer Razer DeathStalker Essential 2014] on usb-.../input0
usb 3-6.2:1.1: razer_deathstalker_fix: synchronized EP0 for secondary interface 1.1
hid-generic 0003:1532:011F.0015: razer_deathstalker_fix: applied NO_INIT_REPORTS | NOGET (quirks=0x20000008)
input: Razer Razer DeathStalker Essential 2014 Keyboard as /devices/.../input/input29
input: Razer Razer DeathStalker Essential 2014 Consumer Control as /devices/.../input/input30
input: Razer Razer DeathStalker Essential 2014 System Control as /devices/.../input/input31
hid-generic 0003:1532:011F.0015: input,hidraw1: USB HID v1.11 Keyboard [Razer Razer DeathStalker Essential 2014] on usb-.../input1
usb 3-6.2:1.2: razer_deathstalker_fix: synchronized EP0 for secondary interface 1.2
hid-generic 0003:1532:011F.0016: razer_deathstalker_fix: applied NO_INIT_REPORTS | NOGET (quirks=0x20000008)
input: Razer Razer DeathStalker Essential 2014 as /devices/.../input/input32
hid-generic 0003:1532:011F.0016: input,hiddev96,hidraw2: USB HID v1.11 Mouse [Razer Razer DeathStalker Essential 2014] on usb-.../input2
```

---

## Installation

### Prerequisites

Install `dkms` and your distribution's kernel headers:

- **Arch Linux / Omarchy:**
  ```bash
  sudo pacman -S --needed dkms base-devel
  # Plus headers matching your kernel (e.g. linux-headers or linux-omarchy-headers)
  ```
- **Debian / Ubuntu:**
  ```bash
  sudo apt install dkms build-essential linux-headers-$(uname -r)
  ```
- **Fedora:**
  ```bash
  sudo dnf install dkms kernel-devel kernel-headers
  ```

### Quick Install (Automatic DKMS + Early-Boot Initramfs)

```bash
git clone https://github.com/Djkawada/razer-deathstalker-linux-fix.git
cd razer-deathstalker-linux-fix
sudo ./install.sh
```

`install.sh` automatically:
1. Registers, compiles, and installs `razer_deathstalker_fix` via **DKMS** (so it survives all future kernel updates).
2. Adds `/etc/modules-load.d/razer-deathstalker-fix.conf`.
3. Adds `razer_deathstalker_fix` to your early-boot **initramfs** (`mkinitcpio`, `dracut`, or `initramfs-tools`) so the keyboard initializes immediately during early boot (including LUKS disk encryption prompts).
4. Loads the module immediately into the running kernel.

### Uninstall

```bash
sudo ./uninstall.sh
```

---

## Module Parameters

- `mode=2` *(default)*: Enables all 3 interfaces (`1.0` Boot Keyboard + LEDs, `1.1` NKRO/Media/System Control, `1.2` Mouse/Vendor Feature Report) with full `EP0` synchronization.
- `mode=0`: Enables only the primary keyboard interface (`1.0`) + LEDs and deauthorizes `1.1` and `1.2`.

---

## License

SPDX-License-Identifier: `GPL-2.0-only`
