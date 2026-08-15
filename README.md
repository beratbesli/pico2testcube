# Pico 2 (RP2350) 125-Cube 3D ASCII Stress Benchmark

Raspberry Pi Pico 2 (RP2350) mikrodenetleyicisi için USB CDC seri portu üzerinden terminale gerçek zamanlı **125 eşzamanlı 3D küp** (5x5x5 Matris) render eden, Z-Buffer derinlik tamponlu donanım stres testi ve benchmark projesi.

## Özellikler

- **Donanım:** Raspberry Pi Pico 2 (RP2350, ARM Cortex-M33 @ 150 MHz, Donanımsal FPU)
- **125 Eşzamanlı Küp:** 5×5×5 kübik uzaysal matris.
- **1.000 Köşe (Vertex) & 1.500 Kenar (Edge):** Her karede donanımsal FPU ile trigonometrik dönüşüm ve Bresenham çizgi rasterizasyonu.
- **ASCII Z-Buffer (Derinlik Tamponu):** Üst üste binen kenarları derinliğe göre sıralar, mesafeye bağlı ASCII karakter yoğunluğu (`@`, `#`, `*`, `+`, `:`, `.`) ile 3D hacim hissi verir.
- **Canlı Mikro-saniye Telemetrisi:**
  - Anlık saf FPU hesaplama süresi ($\mu s$ ve $ms$)
  - Teorik Maksimum Donanım Render Kapasitesi (~FPS)
  - 150 MHz sistem frekansı ve çalışma süresi
- **İletişim:** USB CDC (`stdio_usb`) üzerinden ANSI çift tamponlu, titreşimsiz çıktı.
- **Sıfır Harici Bağımlılık:** SD kart veya harici kütüphane gerektirmez.

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

Derleme çıktısı: `build/pico2_cube_demo.uf2`

## Yükleme ve Çalıştırma

1. Pico 2 üzerindeki **BOOTSEL** butonuna basılı tutarak kartı USB ile bilgisayara bağlayın.
2. Açılan `RPI-RP2` sürücüsüne `pico2_cube_demo.uf2` dosyasını kopyalayın.
3. Terminal üzerinden seri porta bağlanın (Terminal boyutunuzu en az **80x35** yapmanız önerilir):

```bash
# Minicom ile:
minicom -D /dev/ttyACM0 -b 115200

# veya Screen ile:
screen /dev/ttyACM0 115200
```
