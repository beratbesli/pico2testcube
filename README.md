# Pico 2 (RP2350) Out-of-Core Virtual Memory 3D Render Engine

Raspberry Pi Pico 2 (RP2350) mikrodenetleyicisi ve SPI0 üzerinden bağlı MicroSD kart ile **Pico 2'nin RAM sınırını (520 KB) kat kat aşan (1.000.000 bayt / 1 MB)** devasa 3D sanal sahneleri sadece **16 KB RAM** kullanarak işleyen **Sanal Bellek / Sayfalama (LRU Page Cache)** mimarisi.

---

## 🎯 Temel Mimari & Nasıl Çalışır?

* **Devasa Sanal Tuval:** 1000 × 1000 Karakter (1.000.000 Bayt = 1.00 MB / 1954 Sektör).
* **Fiziksel RAM Kullanımı:** Sadece 16 KB (32 adet 512 baytlık LRU sayfa havuzu).
* **İkincil Bellek (Swap):** 2 GB MicroSD Kart (SPI0, 20 MHz Yüksek Hız).
* **Sayfalama Mekanizması:**
  1. Render motoru piksel yazdığında/okuduğunda hedef adresin hangi 512 baytlık SD sektörüne denk geldiği bulunur.
  2. **Cache Hit:** Hedef sektör RAM'deki 32 sayfadan birindeyse anında erişilir.
  3. **Cache Miss:** RAM'deki en eski kullanılmayan sayfa (LRU) eğer değiştirildiyse (dirty) SD karta geri yazılır (Write-Back), ardından talep edilen sektör SD karttan RAM'e yüklenir (Page-In).

---

## 🔌 Donanım & SPI0 Bağlantı Şeması

| MicroSD Kart Modülü | Pico 2 (RP2350) Pini | İşlev |
| :--- | :--- | :--- |
| **MISO (DO)** | **GP16** | SPI0 RX (Dahili Pull-up) |
| **CS (SS)** | **GP17** | Yazılımsal GPIO OUT |
| **SCK (CLK)** | **GP18** | SPI0 Clock (20 MHz) |
| **MOSI (DI)** | **GP19** | SPI0 TX |
| **VCC** | **3V3(OUT)** | 3.3V Besleme |
| **GND** | **GND** | Ortak Toprak |

* **Desteklenen Kartlar:** SDSC (<= 2GB, Byte-Addressed) ve SDHC/SDXC (> 2GB, Block-Addressed) otomatik algılanır.

---

## 📂 Proje Yapısı

```
.
├── .gitignore
├── README.md
└── pico2_cube_demo/
    ├── CMakeLists.txt          # Donanımsal SPI0 ve Pico SDK yapılandırması
    ├── main.c                  # Out-of-Core 3D dünya oluşturucu & Viewport kamera gezgini
    ├── sd_spi.h / sd_spi.c     # Ham, hafif, yüksek hızlı (20MHz) SPI0 SDSC/SDHC sürücüsü
    ├── vmem_fb.h / vmem_fb.c   # 32-slot LRU Page Cache Sanal Bellek yöneticisi
    └── pico_sdk_import.cmake
```

---

## 🛠️ Derleme

```bash
cd pico2_cube_demo
mkdir -p build
cd build
cmake -DPICO_BOARD=pico2 ..
make -j$(nproc)
```

Derleme çıktısı: `build/pico2_cube_demo.uf2`

---

## 🚀 Yükleme ve Çalıştırma

1. Pico 2'yi **BOOTSEL** butonuna basılı tutarak bağlayın.
2. UF2 dosyasını yükleyin:
   ```bash
   cp /home/berat/Desktop/picotest/pico2_cube_demo/build/pico2_cube_demo.uf2 /run/media/$USER/RP2350/
   ```
3. Terminal üzerinden seri porta bağlanın (**80×35** boyut önerilir):
   ```bash
   minicom -D /dev/ttyACM0 -b 115200
   # veya
   screen /dev/ttyACM0 115200
   ```
