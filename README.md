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

iQIzBAABCgAdFiEE3q1u/AX3rRMRccGPoV5C9EdfgFAFAmZmQAoACgkQoV5C9Edf
gFAUKBAAv+khGofRgS9rhvZqPOkHkQ5d5hU3ke05K1n/l0jJwS9d9xXn8r0HXI2D
5u1zJv2+k32K2i9sAtN1Ndf7K1VWyRjPZ/NcK50xHhfZjPYcVyS2q8RH7XMG64Al
hGnZ1Ghsm8n4g6X5rf52QbhWABaX80Beb3+E7we6SpN4vlpyV9DiFaM9Za7zKZSk
zQ==
=abcd
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
