Field Box BT — pachet de distribuție v0.11.0 (8 noiembrie 2026)

app/       fieldbox-bt-0.11.0.apk       aplicația Android (Android 10+); se instalează peste versiunea veche, datele rămân
firmware/  fieldbox_bt_v0_4/             firmware-ul cutiei pentru Arduino IDE (se deschide .ino-ul și se apasă Upload; pas cu pas în Instalare_RO.pdf §5)
           fieldbox-fw-0.4.1-bt.bin     același firmware, imagine completă pentru esptool (se scrie la adresa 0x0 prin USB)
manuale/   Manual_utilizare_RO.pdf      manual de utilizare (română)
           User_manual_EN.pdf           user manual (English)
           Instalare_RO.pdf             ghid de instalare (română)
           Install_EN.pdf               installation guide (English)
CHANGELOG.pdf                           ce s-a schimbat în fiecare versiune

Noutăți în 0.11.0: firmware 0.4.1-bt recunoaște singur senzorii opționali (particule PM1/PM2,5/PM10,
indicii VOC/NOx, TVOC/eCO₂, umiditatea solului, lumina cu BH1750); valorile lor apar în tab-ul Live,
în cardul „Extra sensors”. VEML7700 măsoară acum lumina până la soare direct.
Pentru Arduino IDE, firmware-ul 0.4.1-bt cere în plus bibliotecile BH1750, Adafruit seesaw,
Adafruit SGP30, Sensirion Gas Index Algorithm (Library Manager) și DFRobot_BMV080
(ZIP de pe github.com/DFRobot/DFRobot_BMV080, Sketch > Include Library > Add .ZIP Library).
Imaginea .bin nu cere nimic: se scrie direct.
