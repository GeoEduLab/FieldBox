<p align="center"><img src="logo.png" width="128" alt="Field Box"></p>

# Field Box BT

**English** · [Română](README_RO.md)

**Version 0.8.0** · Android 10 or newer · box firmware 0.3.1-bt · manuals in English and Romanian

## For teachers

Field Box is a kit for environmental measurements on school trips and route
walks. A small sensor box rides in a backpack and measures air temperature,
humidity, pressure, CO₂ and light every 5 seconds. The **Field Box BT** app on
an Android phone talks to it over Bluetooth: it shows the values live, records
the route on a map, reads the box at each station with a photo, and writes
a PDF report of the day that the class can compare between groups.

**The app and the box each have a manual, in English and in Romanian:** a user
manual (every screen, the field day step by step, troubleshooting, a one-page
field card) and an installation guide.

### ⬇️ [Download Field Box 0.8.0 (a single ZIP file)](https://github.com/GeoEduLab/FieldBox/releases/download/v0.8.0/FieldBox_v0.8.0.zip)

The archive holds the app, the box firmware and the four manuals, all from
the same version.

---

## 1. Installing the app on the phone

The app is not on the Play Store; it is installed from the APK file in the
`app` folder.

1. Copy `fieldbox-bt-0.8.0.apk` onto the phone (WhatsApp, a USB cable or
   Google Drive) and tap it.
2. If Android asks, allow installs **from this source**, then tap
   **Install**.
3. If Google Play Protect suggests a scan, tap **Scan app** (or, offline,
   **More details → Install without scanning**). The app is simply not from
   the Play Store; that is all the warning means.
4. On first launch allow the camera, **precise** location and notifications.
5. Pair the box once in **Settings → Bluetooth** (it shows as `BOX_xxxx`, no
   PIN), then in the app tap **Boxes → Connect to a box**.

A newer version installs **over** the old one; the files on the phone stay.
The app itself is in English.

## 2. The box firmware (for the technician)

Boxes as delivered need nothing. To update one, write
`firmware/fieldbox-fw-0.3.1-bt.bin` at address `0x0` over USB (esptool or the
Flash Download Tool; the CH340 driver on Windows). **Download the files from
the box first:** the first start after an update clears its memory. Details in
chapter 5 of the installation guide.

## 3. The manuals

| | English | Română |
|---|---|---|
| User manual | [User_manual_EN.pdf](manuale/User_manual_EN.pdf) | [Manual_utilizare_RO.pdf](manuale/Manual_utilizare_RO.pdf) |
| Installation guide | [Install_EN.pdf](manuale/Install_EN.pdf) | [Instalare_RO.pdf](manuale/Instalare_RO.pdf) |

The same manuals as editable Word files are in the [manuale](manuale) folder.
What changed between versions: [CHANGELOG.pdf](CHANGELOG.pdf).
