# buildroot-rpz2w-airplay

> **Buildroot Tree Updated:** 04/23/2025

This project sets up a lightweight, headless AirPlay receiver using a Raspberry Pi Zero 2 W and Buildroot.

I've tested this and have had success, but this is definitely just a 'for-personal-use-but-ill-throw-it-on-github-for-fun' sort of deal. 

It's hacky in parts, and if you look past the horrifying security practices of using toor as the default root password, storing the wifi PSK in plaintext, among other things, then its not so bad :)

---

## Features

- **Serial Console:**  
  Connect at `115200` baud. (HDMI console testing needed — volunteers welcome!)
  
- **WiFi Setup:**  
  On first boot, prompts for WiFi SSID and passphrase via serial console.

- **Networking:**  
  - Obtains a DHCP lease.
  - Syncs system clock via NTP.

- **Docker Initialization:**  
  - Prepares the system for Docker.
  - Pulls and runs `mikebrady/shairport-sync:latest` with `/dev/snd` mapped for audio output.

---

## Purpose

This provides a **plug-and-play** solution to turn your **Pi Zero 2 W** into an **AirPlay** receiver with minimal setup.

---

## TODO

- [ ] Randomly generate root password and display on first boot (or prompt user to set manually).
- [ ] Utilize `wpa_passphrase` to avoid storing plain-text PSK in `wpa_supplicant.conf`.
- [ ] Add option for manual network configuration.
- [ ] After initial configuration:
  - Remount root filesystem as **read-only**.
  - Provide a CLI tool to toggle writable mode for maintenance.

---


Below is the original README:

Buildroot is a simple, efficient and easy-to-use tool to generate embedded
Linux systems through cross-compilation.

The documentation can be found in docs/manual. You can generate a text
document with 'make manual-text' and read output/docs/manual/manual.text.
Online documentation can be found at http://buildroot.org/docs.html

To build and use the buildroot stuff, do the following:

1) run 'make menuconfig'
2) select the target architecture and the packages you wish to compile
3) run 'make'
4) wait while it compiles
5) find the kernel, bootloader, root filesystem, etc. in output/images

You do not need to be root to build or run buildroot.  Have fun!

Buildroot comes with a basic configuration for a number of boards. Run
'make list-defconfigs' to view the list of provided configurations.

Please feed suggestions, bug reports, insults, and bribes back to the
buildroot mailing list: buildroot@buildroot.org
You can also find us on #buildroot on OFTC IRC.

If you would like to contribute patches, please read
https://buildroot.org/manual.html#submitting-patches
