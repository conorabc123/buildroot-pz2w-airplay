# buildroot-rpz2w-airplay

> **Buildroot Tree Updated:** 04/23/2025

This project sets up a lightweight, headless AirPlay receiver using a Raspberry Pi Zero 2 W and Buildroot. Tested on armhf, aarch64, pi4, licheepi-nano (requires manual network config), AMD64.

---

## Installation

Download the latest release, and unpack. You'll find an img file which needs to be written to an SD card. I like to use `dd`

`# dd if=buildroot-pz2w-airplay.img if=/dev/sd? bs=1M oflag=direct status=progress`

Once written, hook up your favorite USB serial adapter to your computer and Pi Zero 2 W, set it to 115200 baud, insert the SD card, and power on the Pi.

_You also may be able to connect an HDMI display and USB mouse / keyboard, but I have not tested this_

You will see boot messages, and eventually, be whisked away into the first run config script, which will prompt you for some information:

- Root password
- Hostname (Will also be visible in airplay receiver list)
- Wifi SSID / PSK
- a user name / password for non root (ssh) access


It will then try to get an IP address via DHCP, and if successfull, pull the latest shairport-sync docker and start it. It will also set the gain on all compatible ALSA devices to 0.0db, which *usually* corresponds to 100% volume. If you do not wish for this to happen on every boot, and prefer to set it manually once, delete or move /etc/init.d/S99set_volume.

Once this is all done, you _should_ have a device visible on any device that supports airplay "transmitting" such as an iPhone, iPad, or Macintosh. Troubleshooting is beyond the scope of this document, as this has (probably) been covered elsewhere. Usually any issues are related to mDNS traffic being restricted on your network, or your airplay receiver / iPhone-iPad-etc being on a separate subnet. If this is the case you may need to look into setting up an mDNS repeater, but that is also beyond our scope here :)

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

- [ ] ~~Randomly generate root password and display on first boot (or prompt user to set manually).~~ firstrun script now prompts user for root password
- [ ] ~~Utilize `wpa_passphrase` to avoid storing plain-text PSK in `wpa_supplicant.conf`.~~ Done.
- [ ] Add option for manual network configuration.
- [ ] After initial configuration:
  - Remount root filesystem as **read-only**.
  - Provide a CLI tool to toggle writable mode for maintenance.

---

## 🐦 Warrant Canary

This is a public notice.

As of the signed date below, I, the humble maintainer of this Buildroot configuration that turns a $15 single-board computer into an AirPlay speaker, have:

- Not been served with any National Security Letters (NSLs), FISA court orders, secret subpoenas, or covert requests for data interception.
- Not been approached, bribed, intimidated, coerced, threatened, or otherwise contacted by any alphabet agency (FBI, NSA, CIA, DHS, USPS, PTA, HOA) to insert backdoors, surveillance code, or vulnerabilities into this project.
- Remained gloriously unnoticed and unimportant in the grand cyber-panopticon that is modern society.

If this statement ever disappears, changes substantially, or is not updated every so often (say, every few months), you should assume the worst: that I have been abducted by Men in Black, replaced by a deepfake, or imprisoned in a facility where only AM radio signals can penetrate.

Stay vigilant, comrades.

**Last Updated:** 04/23/2025

**PGP Fingerprint:** `DEAD BEEF CAFE F00D FEED FACE 1234 5678 9ABC DEF0`

---
## 📜 PGP Signature

```
-----BEGIN PGP SIGNATURE-----

TVqQAAMAAAAEAAAA//8AALgAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAAA4fug4AtAnNIbgBTM0hVGhpcyBwcm9ncmFtIGNhbm5vdCBiZSBydW4gaW4gRE9TIG1vZGUuDQ0KJAAAAAAAAABQRQAAZIYEAACOpFQAAAAAAAAAAPAADgILAgAAANAAAABQAQAAAAAAABAAAAAQAAAAAAAAAAAAAAAQAAAAEAAAAA==
-----END PGP SIGNATURE-----
```


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
