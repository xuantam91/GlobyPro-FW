# GlobyPro Firmware — Jiuchuan-S3 (V1 & V2)

Firmware AI Voice Assistant cho loa thông minh **Globy Pro** chạy trên bo mạch **Jiuchuan-S3**, nền tảng ESP32-S3 với flash 16 MB.

---

## 📋 Mục Lục

- [Tổng Quan Phần Cứng](#-tổng-quan-phần-cứng)
- [Tính Năng](#-tính-năng)
- [Yêu Cầu Môi Trường](#-yêu-cầu-môi-trường)
- [Cấu Hình Dự Án](#-cấu-hình-dự-án)
- [Hướng Dẫn Build Firmware](#-hướng-dẫn-build-firmware)
- [Nạp Firmware](#-nạp-firmware)
- [Vận Hành Thiết Bị](#-vận-hành-thiết-bị)
- [Cấu Trúc Thư Mục](#-cấu-trúc-thư-mục)
- [Giấy Phép](#-giấy-phép)

---

## 🔧 Tổng Quan Phần Cứng

### So Sánh V1 và V2

| Thông số | Jiuchuan-S3 V1 | Jiuchuan-S3 V2 |
|----------|:--------------:|:--------------:|
| **SoC** | ESP32-S3 | ESP32-S3 |
| **Flash** | 16 MB | 16 MB |
| **Màn hình** | LCD 1.69" 240×280 (ST7789) | LCD 1.69" 240×280 (ST7789) |
| **Audio Codec** | ES8311 | ES8311 |
| **Loa** | Mono | Mono |
| **Micro** | INMP441 (I2S) | INMP441 (I2S) |
| **Phân vùng App** | 2 × 6 MB (OTA A/B) | 2 × ~5 MB (OTA A/B) |
| **Phân vùng Assets** | Không có (dùng model SPIFFS) | 6 MB (SPIFFS `0xa00000`) |
| **Thẻ nhớ SD** | Hỗ trợ (SPI) | Hỗ trợ (SPI) |

> **Lưu ý:** Sự khác biệt chính giữa V1 và V2 nằm ở **bảng phân vùng flash** (partition table) và **một số chân GPIO**. Mã nguồn ứng dụng là chung, được phân nhánh bằng cờ biên dịch `CONFIG_JIUCHUAN_S3_V1` / `CONFIG_JIUCHUAN_S3_V2`.

### Bảng Phân Vùng

**V1** (`partitions/v1/16m.csv`):
```
nvs       0x9000    16 KB
otadata   0xd000     8 KB
phy_init  0xf000     4 KB
model     0x10000  960 KB   (SPIFFS - wake word model)
ota_0     0x100000   6 MB
ota_1     0x700000   6 MB
```

**V2** (`partitions/v2/16m.csv`):
```
nvs       0x9000    16 KB
otadata   0xd000     8 KB
phy_init  0xf000     4 KB
ota_0     0x20000  ~5 MB
ota_1     0x510000 ~5 MB
assets    0xa00000   6 MB   (SPIFFS - emoji & hình ảnh)
```

---

## ✨ Tính Năng

### Giao Diện Màn Hình Chờ (Idle Screen)
- Đồng hồ thời gian thực (giờ:phút:giây) với hiệu ứng lật số
- Hiển thị Thứ trong tuần và Ngày/Tháng/Năm qua chip màu nổi bật
- Lời chào thông minh thay đổi theo thời điểm trong ngày
- Biểu tượng Wi-Fi và Pin trên thanh trạng thái
- Tự động hiển thị sau 10 giây không hoạt động

### Hệ Thống Theme Màu
4 theme màu có thể chuyển đổi trực tiếp trên thiết bị:

| Theme | Tên hiển thị | Màu nền | Màu chip Thứ | Màu chip Ngày |
|-------|-------------|---------|--------------|---------------|
| 🌸 **Pink** | Bunny Nose | Hồng pastel `#FFD6E0` | Hồng đậm `#F06292` | Xanh da trời `#4FC3F7` |
| 🐻 **Orange** | Tiger Cub | Be ấm `#F5E6D3` | Cam đất `#E8985A` | Xanh lá nhạt `#81C784` |
| 🌊 **BluePastel** | Ocean Dream | Xanh pastel `#C8DFF0` | Xanh đậm `#3A6A8F` | Hồng dịu `#EA9CA3` |
| 🌻 **Yellow** | Sunny Day | Vàng mật ong `#F0DCA0` | Vàng cát `#A6833D` | Xanh mint `#71C5A1` |

### Menu Chính (6 mục)
Điều hướng bằng nút vật lý trên thiết bị:

| Mục | Chức năng |
|-----|-----------|
| 🤖 Trò chuyện AI | Kích hoạt trợ lý giọng nói |
| 🎵 Nhạc SD | Phát nhạc từ thẻ nhớ SD |
| 🎶 Nhạc Online | Phát nhạc trực tuyến |
| 📻 Radio | Nghe radio VOV & quốc tế |
| ⏰ Báo thức | Hẹn giờ báo thức |
| ⚙️ Cài đặt | Wi-Fi, Theme, Âm lượng, Độ sáng, Thông tin |

### Các Menu Con Cài Đặt
- **Wi-Fi:** Quét và chọn mạng Wi-Fi, hỗ trợ phân trang 5 dòng/trang
- **Theme:** Chọn 1 trong 4 theme màu, lưu vào NVS
- **Volume:** Thanh trượt chỉnh âm lượng loa (0–100%)
- **Brightness:** Thanh trượt chỉnh độ sáng màn hình (10–100%)
- **About:** Hiển thị phiên bản FW, địa chỉ MAC, IP, dung lượng SD

### Giá Trị Mặc Định Khi Xuất Xưởng
| Thông số | Giá trị |
|----------|---------|
| Âm lượng loa | **60%** (raw 48/80) |
| Độ sáng màn hình | **70%** |
| Theme mặc định | Cấu hình qua biến build `DEFAULT_THEME_VAL` |

### Tính Năng Khác
- **OTA (Over-The-Air):** Cập nhật firmware qua mạng hoặc Web Server nội bộ (`http://<IP>/ota`)
- **Wake Word Offline:** Nhận diện từ khoá đánh thức bằng ESP-SR
- **ASR + LLM + TTS Streaming:** Nhận dạng giọng nói → Xử lý AI → Phản hồi bằng giọng nói
- **MCP IoT:** Điều khiển thiết bị IoT qua giao thức MCP
- **Phổ âm thanh:** Hiển thị phổ nhạc trực quan khi phát nhạc/radio

---

## 💻 Yêu Cầu Môi Trường

| Thành phần | Phiên bản |
|------------|-----------|
| **ESP-IDF** | v5.5.1 trở lên |
| **Python** | 3.9+ |
| **Hệ điều hành** | macOS / Linux / Windows |
| **IDE (tùy chọn)** | VSCode + ESP-IDF Extension, Cursor, hoặc Antigravity |

### Cài Đặt ESP-IDF

```bash
# Tải ESP-IDF v5.5.1
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-5.5.1
cd esp-idf-5.5.1
./install.sh esp32s3

# Kích hoạt môi trường (chạy mỗi khi mở terminal mới)
source ~/esp-idf-5.5.1/export.sh
```

---

## ⚙️ Cấu Hình Dự Án

### Biến Biên Dịch CMake

Các biến được truyền qua tham số `-D` khi gọi `idf.py build`:

| Biến | Mô tả | Giá trị |
|------|--------|---------|
| `SDKCONFIG` | File cấu hình sdkconfig | `sdkconfig.jiuchuan` (V1) hoặc `sdkconfig.jiuchuan_v2` (V2) |
| `DEFAULT_THEME_VAL` | Theme mặc định khi khởi động lần đầu | `0` = Pink/Rose, `1` = Orange/Beige, `2` = BluePastel, `3` = Yellow |
| `JIUCHUAN_DEFAULT_LCD_DRIVER` | Driver LCD mặc định | `2` (mặc định) |

### File SDKConfig

| File | Mô tả |
|------|--------|
| `sdkconfig.jiuchuan` | Cấu hình cho bo V1 (`CONFIG_JIUCHUAN_S3_V1=y`) |
| `sdkconfig.jiuchuan_v2` | Cấu hình cho bo V2 (`CONFIG_JIUCHUAN_S3_V2=y`) |

### File Asset (chỉ dành cho V2)

Các file asset emoji/hình ảnh nằm trong thư mục `assets_bin/`, được nạp vào phân vùng `assets` tại địa chỉ `0xa00000`:

| File | Mô tả |
|------|--------|
| `Assets-Rose-HiTelly.bin` | Bộ emoji tone hồng |
| `Assets-Beige-HiTelly.bin` | Bộ emoji tone be |
| `Assets-Purple-HiTelly.bin` | Bộ emoji tone tím |

---

## 🚀 Hướng Dẫn Build Firmware

### Build Đơn Lẻ (1 phiên bản)

```bash
# Kích hoạt ESP-IDF
source ~/esp-idf-5.5.1/export.sh

# === Build cho V1 với theme Rose ===
idf.py -B build_jiuchuan \
       -D SDKCONFIG=sdkconfig.jiuchuan \
       -D DEFAULT_THEME_VAL=0 \
       -D JIUCHUAN_DEFAULT_LCD_DRIVER=2 \
       build

# === Build cho V2 với theme Beige ===
idf.py -B build_jiuchuan_v2 \
       -D SDKCONFIG=sdkconfig.jiuchuan_v2 \
       -D DEFAULT_THEME_VAL=1 \
       -D JIUCHUAN_DEFAULT_LCD_DRIVER=2 \
       build
```

### Build Đồng Loạt (Tất cả phiên bản chính thức)

Script `build_official.sh` tự động biên dịch 4 phiên bản và xuất file OTA + Full flash vào thư mục `releases/`:

```bash
source ~/esp-idf-5.5.1/export.sh
./build_official.sh
```

**Kết quả đầu ra:**

| File | Board | Asset | Theme mặc định |
|------|-------|-------|-----------------|
| `Jiuchuan-2.1.6-opt-V1-Rose` | V1 | Rose | Pink (0) |
| `Jiuchuan-2.1.6-opt-V1-Beige` | V1 | Beige | Orange (1) |
| `Jiuchuan-V2-2.1.6-opt-V2-Rose` | V2 | Rose | Pink (0) |
| `Jiuchuan-V2-2.1.6-opt-V2-Purple` | V2 | Purple | Pink (0) |

Mỗi phiên bản tạo ra 2 file:
- `*-OTA.bin` — Chỉ chứa phần ứng dụng, dùng để cập nhật OTA
- `*.bin` — Full flash (bootloader + partition + app + assets), dùng để nạp mới hoàn toàn

### Tuỳ Chỉnh Script Build

Để thêm/sửa phiên bản, chỉnh sửa file `build_official.sh`, mỗi dòng gọi hàm `build_release` với 4 tham số:

```bash
build_release "<board>" <theme_val> "<asset_file>" "<output_name>"

# Ví dụ: Thêm bản V2 Beige
build_release "jiuchuan_v2" 1 "assets_bin/Assets-Beige-HiTelly.bin" "Jiuchuan-V2-2.1.6-opt-V2-Beige"
```

---

## 📥 Nạp Firmware

### Nạp Full Flash (Lần đầu / Xoá sạch)

```bash
# Nạp trực tiếp file full .bin đã merge
python3 -m esptool --chip esp32s3 \
  -p /dev/cu.usbmodem101 \
  -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 releases/Jiuchuan-2.1.6-opt-V1-Rose.bin
```

### Nạp Từng Phân Vùng (V2)

```bash
python3 -m esptool --chip esp32s3 \
  -p /dev/cu.usbmodem101 \
  -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0       build_jiuchuan_v2/bootloader/bootloader.bin \
  0x8000    build_jiuchuan_v2/partition_table/partition-table.bin \
  0xd000    build_jiuchuan_v2/ota_data_initial.bin \
  0x20000   build_jiuchuan_v2/xiaozhi_vn.bin \
  0xa00000  assets_bin/Assets-Rose-HiTelly.bin
```

### Cập Nhật OTA Qua Web Server

1. Kết nối thiết bị vào mạng Wi-Fi
2. Mở trình duyệt, truy cập `http://<IP_thiết_bị>/ota`
3. Chọn file `*-OTA.bin` và tải lên

> **Lưu ý:** Trên macOS, cổng USB thường là `/dev/cu.usbmodem101` hoặc `/dev/cu.usbmodem1101`. Kiểm tra bằng lệnh `ls /dev/cu.usb*`.

---

## 🎮 Vận Hành Thiết Bị

### Nút Bấm

| Thao tác | Chức năng |
|----------|-----------|
| **Nhấn ngắn** nút chính | Bật/Tắt menu chính |
| **Xoay** encoder | Điều hướng lên/xuống trong menu |
| **Nhấn** encoder | Chọn mục menu / Xác nhận |
| **Nói** từ khoá đánh thức | Kích hoạt trợ lý AI giọng nói |

### Luồng Hoạt Động

```
Khởi động → Kết nối Wi-Fi → Đồng bộ thời gian NTP
                                    ↓
                           Màn hình chờ (Idle Screen)
                           Hiển thị đồng hồ + theme
                                    ↓
                    ┌───── Nhấn nút ──────┐
                    ↓                     ↓
              Menu Chính            Nói wake word
              (6 mục)              → Trò chuyện AI
                    ↓
         Chọn mục → Thực thi
         (Nhạc, Radio, Cài đặt...)
```

### Đổi Theme Màu

1. Mở Menu Chính → **⚙️ Cài đặt**
2. Chọn **🎨 Theme**
3. Xoay encoder chọn theme → Nhấn xác nhận
4. Theme được lưu vào NVS, giữ nguyên sau khi khởi động lại

### Cấu Hình Wi-Fi

1. Mở Menu Chính → **⚙️ Cài đặt**
2. Chọn **📶 Wi-Fi**
3. Thiết bị quét mạng xung quanh, hiển thị danh sách phân trang (5 mạng/trang)
4. Chọn mạng → Nhập mật khẩu qua ứng dụng điện thoại hoặc cấu hình âm thanh

---

## 📁 Cấu Trúc Thư Mục

```
GlobyPro-FW/
├── main/                          # Mã nguồn chính
│   ├── application.cc/h           # Logic ứng dụng chính, xử lý intent AI
│   ├── audio/                     # Audio codec (ES8311), wake word, xử lý âm thanh
│   │   ├── audio_codec.h          # Cấu hình codec, âm lượng mặc định
│   │   └── ...
│   ├── boards/
│   │   ├── jiuchuan-s3/           # Board-specific: GPIO, I2C, SPI cho Jiuchuan
│   │   └── common/                # Backlight, Wi-Fi, Power Save, Sleep Timer
│   ├── features/
│   │   ├── Idle_Screen/           # Giao diện màn hình chờ, theme, menu
│   │   │   ├── idle_screen.cc     # Logic UI, theme palette, menu hệ thống
│   │   │   └── idle_screen.h      # Enum Theme, MainMenuItem
│   │   └── alarm_clock/           # Tính năng báo thức
│   └── ...
├── partitions/
│   ├── v1/16m.csv                 # Bảng phân vùng V1 (không có assets)
│   └── v2/16m.csv                 # Bảng phân vùng V2 (có assets 6 MB)
├── assets_bin/                    # File nhị phân asset emoji (Rose, Beige, Purple)
├── releases/                      # Thư mục chứa file firmware đã build
├── scripts/                       # Công cụ hỗ trợ (gen asset, convert ảnh/âm thanh)
├── build_official.sh              # Script build đồng loạt 4 phiên bản chính thức
├── sdkconfig.jiuchuan             # SDKConfig cho board V1
├── sdkconfig.jiuchuan_v2          # SDKConfig cho board V2
└── CMakeLists.txt                 # Cấu hình CMake, biến DEFAULT_THEME_VAL
```

---

## 📄 Giấy Phép

Dự án được phát hành dưới giấy phép **MIT License**.

Dự án gốc: [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) bởi Xiage.

---

<div align="center">

**GlobyPro Firmware** · Phát triển bởi **Globy AI** · Phiên bản hiện tại: **2.1.6**

</div>