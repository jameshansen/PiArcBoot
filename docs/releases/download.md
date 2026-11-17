## Download

| | Installer | Portable | |
|---|---|---|---|
| <img src="https://raw.githubusercontent.com/jameshansen/PiArcBoot/main/docs/images/os-raspberrypi.png" width="32" height="32" alt=""> **Raspberry Pi** | [PiArcBoot-Installer-RaspberryPi.zip](https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}/PiArcBoot-Installer-RaspberryPi.zip) | [PiArcBoot-Portable-RaspberryPi.zip](https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}/PiArcBoot-Portable-RaspberryPi.zip) | 64-bit Raspberry Pi OS 13 (trixie) or newer |
| <img src="https://raw.githubusercontent.com/jameshansen/PiArcBoot/main/docs/images/os-windows.png" width="32" height="32" alt=""> **Windows** | [PiArcBoot-Installer-Windows.zip](https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}/PiArcBoot-Installer-Windows.zip) | [PiArcBoot-Portable-Windows.zip](https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}/PiArcBoot-Portable-Windows.zip) | 64-bit Windows |
| <img src="https://raw.githubusercontent.com/jameshansen/PiArcBoot/main/docs/images/os-linux.png" width="32" height="32" alt=""> **Linux** | [PiArcBoot-Installer-Linux-x86-64.zip](https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}/PiArcBoot-Installer-Linux-x86-64.zip) | [PiArcBoot-Portable-Linux-x86-64.zip](https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}/PiArcBoot-Portable-Linux-x86-64.zip) | A 64-bit PC desktop as new as Ubuntu 24.04 or Debian 13 |
| <img src="https://raw.githubusercontent.com/jameshansen/PiArcBoot/main/docs/images/os-macos.png" width="32" height="32" alt=""> **Mac** | [PiArcBoot-macOS.zip](https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}/PiArcBoot-macOS.zip) | | macOS 10.13 or newer, Apple silicon or Intel |

Extract the installer and run `Install PiArcBoot`. A portable copy runs from wherever it's extracted and keeps its games and saves in its own `Data` folder. On a Mac, open the zip and drag PiArcBoot to Applications. The first time, macOS says it can't check PiArcBoot: open System Settings, then Privacy & Security, and choose Open Anyway.

Or install from a terminal, which on a Mac needs no Open Anyway. On a Raspberry Pi, a Linux PC or a Mac:

```bash
curl -fsSL https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}/install.sh | sh
```

On Windows, in PowerShell:

```powershell
irm https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}/install.ps1 | iex
```

---
