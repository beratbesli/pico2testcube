# Pico 2 (RP2350) 3D ASCII Rotating Cube Demo

Raspberry Pi Pico 2 (RP2350) mikrodenetleyicisi için USB CDC seri portu üzerinden terminale gerçek zamanlı 3D döner küp render eden bağımsız demo projesi.

## Özellikler

- **Donanım:** Raspberry Pi Pico 2 (RP2350, ARM Cortex-M33 @ 150 MHz, Donanımsal FPU)
- **Sıfır Dış Bağımlılık:** SD kart veya harici dosya sistemi gerektirmez, yalnızca temel Pico SDK kullanılır.
- **İletişim:** USB CDC (`stdio_usb`) üzerinden 115200 baud seri haberleşme.
- **3D Render:**
  - 3 eksenli (Pitch, Yaw, Roll) gerçek zamanlı trigonometrik rotasyon.
  - Bresenham çizgi algoritması ile ASCII tel kafes (wireframe) çizimi.
  - ANSI escape kodları (`\033[2J\033[H`) ile çift tamponlu, titreşimsiz çıktı.
  - ~20 FPS (50 ms) kararlı kare hızı.
- **Dahili Telemetri:** Çalışma süresi, kare sayısı, saat frekansı ve LED kalp atışı (heartbeat).

## Proje Yapısı

```
.
├── .gitignore
├── README.md
└── pico2_cube_demo/
    ├── CMakeLists.txt
    ├── main.c
    └── pico_sdk_import.cmake
```

## Derleme

```bash
cd pico2_cube_demo
mkdir -p build
cd build
cmake -DPICO_BOARD=pico2 ..
make -j$(nproc)
```

Derleme sonucunda `build/pico2_cube_demo.uf2` dosyası üretilir.

## Yükleme ve Çalıştırma

1. Pico 2 üzerindeki **BOOTSEL** butonuna basılı tutarak kartı USB ile bilgisayara bağlayın.
2. Oluşan `RPI-RP2` sürücüsüne `pico2_cube_demo.uf2` dosyasını kopyalayın.
3. Terminal üzerinden seri porta bağlanın:

```bash
# Minicom ile:
minicom -D /dev/ttyACM0 -b 115200

# veya Screen ile:
screen /dev/ttyACM0 115200
```
