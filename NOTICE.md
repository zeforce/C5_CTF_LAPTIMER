# Attribution and third-party notices

## C5VRX (GPL-3.0-only)

Original project: https://github.com/Twotoz/C5VRX
Authors: Twotoz and the C5VRX contributors.
Reference revision: 100bf7bfa34e0892a3ab1efd856564d0df37e92a.
Official project site: https://twotoz.github.io/C5VRX/

`firmware/c5_detector/main/rf_capture.c` adapts `main/rf.c` (continuous MODEM_DIAG receive setup and IQ routing) and `main/transport.c` (PARLIO RX configuration). Modifications include an RX-only infinite PARLIO transaction, detector-oriented sampled power/quality statistics, configurable channel/gain and scan support, and removal of video TX/DAC/menu paths. The file retains its upstream attribution. New detector/controller/SPI/network/game functionality is distributed under GPL-3.0-only together with this derivative. Publication snapshot: 2026-11-23. This project is not an official C5VRX release or endorsement.

## Gameplay and interaction references

SeekND/CaptureTheFlag by Carlos Costa / SeekND and contributors: https://github.com/SeekND/CaptureTheFlag , studied revision b5de9b794e0d2f3903c35ddcda7af0925f34560f (0.8a source contains MIT notices). Gameplay concepts were studied; no upstream program source is included in this publication.

LouisHitchcock/FPVGate: https://github.com/LouisHitchcock/FPVGate . Configuration and signal-chart interaction concepts were consulted; no program source or UI assets from it are included here.

## External SDK

ESP-IDF v6.0.2: https://github.com/espressif/esp-idf . Its components, bundled third-party code and binary libraries retain their own licenses, including Apache-2.0 and component-specific terms. This repository does not vendor the SDK or PHY archive and does not relicense those dependencies. Review their notices when redistributing SDK material or firmware. See docs/build.md and docs/licensing.md.
