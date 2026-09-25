# OS08E10 kernel driver for Raspberry Pi

[![Build](https://github.com/Kurokesu/os08e10-rpi-driver/actions/workflows/build.yml/badge.svg)](https://github.com/Kurokesu/os08e10-rpi-driver/actions/workflows/build.yml)
[![Code style](https://github.com/Kurokesu/os08e10-rpi-driver/actions/workflows/code-style.yml/badge.svg)](https://github.com/Kurokesu/os08e10-rpi-driver/actions/workflows/code-style.yml)
[![Release](https://img.shields.io/github/v/release/Kurokesu/os08e10-rpi-driver)](https://github.com/Kurokesu/os08e10-rpi-driver/releases/latest)
[![Kurokesu apt archive](https://img.shields.io/badge/apt-Kurokesu_archive-D70A53?logo=debian)](https://apt.kurokesu.com)
[![RPi OS Bookworm | Trixie](https://img.shields.io/badge/RPi_OS-Bookworm_%7C_Trixie-blue?logo=raspberrypi)](https://www.raspberrypi.com/software/operating-systems/)
[![Kernel 6.12+](https://img.shields.io/badge/kernel-6.12%2B-blue?logo=raspberrypi)](https://github.com/raspberrypi/linux/tree/rpi-6.12.y)

Raspberry Pi kernel driver for OmniVision OS08E10, an 8 MP rolling shutter 1/1.818" CMOS sensor built on OmniVision PureCel Plus pixel technology with Nyxel near-infrared response.

- 2-lane and 4-lane MIPI CSI-2 (up to 1452 Mbps/lane)
- 10-bit and 12-bit RAW output
- 3840×2160 @ 60 fps (10-bit, 4-lane)
- 3840×2160 @ 30 fps (12-bit, 4-lane)

![Kurokesu camera modules connected to a Raspberry Pi 5](https://raw.githubusercontent.com/Kurokesu/os08e10-rpi-driver/main/docs/kurokesu-on-pi.jpg)

*OS08E10 camera modules are available at [kurokesu.com](https://www.kurokesu.com/item/CAM-CSI)*

## Install

Connect camera to CSI port with Pi powered off.

Update OS and reboot:

```bash
sudo apt update && sudo apt full-upgrade -y
sudo reboot
```

> [!IMPORTANT]
> If driver or camera stack was previously built from source, run one-time cleanup before first apt install. See [migrating from a source install](#migrating-from-a-source-install).

Enable Kurokesu apt archive (skip if already enabled):

```bash
curl -fsSLO https://apt.kurokesu.com/setup.sh
sudo sh setup.sh
```

Install driver and camera stack:

```bash
sudo apt update
sudo apt install -y os08e10-rpi-dkms rpicam-apps
```

*With archive enabled, apt resolves Kurokesu `rpicam-apps` and `libcamera` forks with OS08E10 support as updates to stock packages. Later updates arrive with regular `apt upgrade`.*

Edit boot configuration:

```bash
sudo nano /boot/firmware/config.txt
```

Make two changes:

1. Find `camera_auto_detect` near the top and set it to `0`:

```ini
camera_auto_detect=0
```

2. Add `dtoverlay=os08e10` under the `[all]` section at the bottom of the file:

```ini
[all]
dtoverlay=os08e10
```

*If camera is connected to cam0 port, use `dtoverlay=os08e10,cam0` instead. See [cam0](#cam0).*

Save and exit.

`config.txt` changes take effect after reboot:

```bash
sudo reboot
```

Verify camera is detected:

```bash
rpicam-hello --list-cameras
```

Expected output (varies by link frequency and lane configuration):

```
Available cameras
-----------------
0 : os08e10 [3840x2160 12-bit] (/base/axi/pcie@1000120000/rp1/i2c@70000/os08e10@3c)
    Modes: 'SBGGR10_CSI2P' : 3840x2160 [30.01 fps - (0, 0)/3840x2160 crop]
           'SBGGR12_CSI2P' : 3840x2160 [15.00 fps - (0, 0)/3840x2160 crop]
```

Start live preview:

```bash
rpicam-hello -t 0
```

On headless systems, capture a still image instead:

```bash
rpicam-still -o test.jpg
```

## dtoverlay options

`os08e10` overlay supports comma-separated options to override defaults:

| option | description | default |
|--------|-------------|---------|
| [`cam0`](#cam0) | Use cam0 port instead of cam1 | cam1 |
| [`4lane`](#4lane) | Use 4-lane MIPI CSI-2 (if wired) | 2 lanes |
| [`link-frequency=<Hz>`](#link-frequency) | Set MIPI CSI-2 link frequency (Hz) | 726000000 |
| [`always-on`](#always-on) | Keep regulator powered (prevents runtime PM power-off) | off |

### cam0

If camera is connected to cam0 port, append `,cam0`:

```ini
dtoverlay=os08e10,cam0
```

### 4lane

To enable 4-lane MIPI CSI-2, append `,4lane`:

```ini
dtoverlay=os08e10,4lane
```

> [!WARNING]
> Before using `4lane`, confirm your camera port actually supports 4-lane MIPI CSI. Not all Raspberry Pi models and carrier boards provide 4-lane MIPI CSI on both ports.

### link-frequency

Supported link frequencies: 726 MHz (default) and 369 MHz.

To set link frequency to 369 MHz, append `,link-frequency=369000000`:

```ini
dtoverlay=os08e10,link-frequency=369000000
```

#### Output formats

| Link frequency | Data rate / lane | Lanes | Bit depth | Width | Height | Max FPS |
|---|---|---|---|---|---|---|
| **4K UHD 2160p (full resolution)** | | | | | | |
| 369 MHz | 738 Mbps | 2 | 10 | 3840 | 2160 | 15 fps |
| 369 MHz | 738 Mbps | 2 | 12 | 3840 | 2160 | 7.5 fps |
| 369 MHz | 738 Mbps | 4 | 10 | 3840 | 2160 | 30 fps |
| 369 MHz | 738 Mbps | 4 | 12 | 3840 | 2160 | 15 fps |
| 726 MHz | 1452 Mbps | 2 | 10 | 3840 | 2160 | 30 fps |
| 726 MHz | 1452 Mbps | 2 | 12 | 3840 | 2160 | 15 fps |
| 726 MHz | 1452 Mbps | 4 | 10 | 3840 | 2160 | 60 fps |
| 726 MHz | 1452 Mbps | 4 | 12 | 3840 | 2160 | 30 fps |

> [!NOTE]
> 60 fps is raw capture only. Raspberry Pi ISP processes 3840×2160 at up to 30 fps.

> [!TIP]
> On Raspberry Pi 4 and CM4, CPU frequency scaling lowers ISP clock during 4K capture and drops frames at 30 fps. Add `force_turbo=1` to `config.txt` to hold clocks.

> [!TIP]
> Options can be combined. Example (cam0, 4-lane, 369 MHz):
> ```ini
> dtoverlay=os08e10,cam0,4lane,link-frequency=369000000
> ```

### always-on

`always-on` keeps camera regulator permanently enabled, preventing kernel from powering off sensor during runtime PM suspend.

```ini
dtoverlay=os08e10,always-on
```

## Build from source

Install required tools:

```bash
sudo apt install -y git
sudo apt install -y --no-install-recommends dkms
```

Clone this repository:

```bash
cd ~
git clone https://github.com/Kurokesu/os08e10-rpi-driver.git
cd os08e10-rpi-driver/
```

If driver was installed from apt archive previously, remove it first:

```bash
sudo apt remove os08e10-rpi-dkms
```

Run setup script:

```bash
sudo ./setup.sh
```

Camera stack, boot configuration and verification follow [Install](#install). Skip `os08e10-rpi-dkms` there, only `rpicam-apps` is needed. To build `libcamera` and `rpicam-apps` from source as well, see [libcamera/BUILDING.md](https://github.com/Kurokesu/libcamera/blob/kurokesu/BUILDING.md).

## Migrating from a source install

One-time cleanup before first apt install.

Remove `os08e10` driver modules installed by `setup.sh`:

```bash
dkms status | grep os08e10 | cut -d, -f1 | sort -u | xargs -rI{} sudo dkms remove {} --all
```

Source-built `libcamera` and `rpicam-apps` install to `/usr/local` and shadow packaged binaries. Remove them:

> [!WARNING]
> Command below deletes everything under `/usr/local` with `libcamera`, `rpicam` or `libpisp` in its name, including custom scripts or files named after them.

```bash
sudo find /usr/local -depth \( -name '*libcamera*' -o -name '*rpicam*' -o -name '*libpisp*' \) -exec rm -rf {} +
```

Cleanup complete. Continue with [install steps](#install).
