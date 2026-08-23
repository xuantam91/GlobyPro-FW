# Hướng dẫn build firmware `.bin` theo cấu hình `jiuchuan` và `luxiaoban`

> Lưu ý: bạn ghi `luaxiaoban`, trong repo hiện có file cấu hình là `sdkconfig.luxiaoban` (tên đúng: **luxiaoban**).

## 1) Chuẩn bị môi trường ESP-IDF

Chạy trong terminal (macOS/Linux):

```bash
# 1. Vào thư mục project
cd /Users/tamtran/Downloads/xiaozhi-esp32_vietnam-develop_vn-2

# 2. Nạp môi trường ESP-IDF (điều chỉnh đường dẫn nếu IDF của bạn khác)
source ~/esp/esp-idf/export.sh

# 3. Kiểm tra idf.py
idf.py --version
```

Nếu lệnh `idf.py` chạy được là OK.

## 2) Build theo config `jiuchuan`

### 2.1 Build app

```bash
cd /Users/tamtran/Downloads/xiaozhi-esp32_vietnam-develop_vn-2
source ~/esp/esp-idf/export.sh

# Build riêng vào thư mục build_jiuchuan để không đè bản khác
idf.py -B build_jiuchuan \
  -D SDKCONFIG=sdkconfig.jiuchuan \
  -D JIUCHUAN_DEFAULT_LCD_DRIVER=2 \
  build
```

Ghi chú:
- `SDKCONFIG=sdkconfig.jiuchuan`: dùng đúng cấu hình Jiuchuan.
- `JIUCHUAN_DEFAULT_LCD_DRIVER=2`: mặc định màn hình mới (JD9853).

### 2.2 Tạo OTA bin và merged bin

```bash
cd /Users/tamtran/Downloads/xiaozhi-esp32_vietnam-develop_vn-2

# OTA bin (chỉ app)
cp build_jiuchuan/xiaozhi_vn.bin Globy-RabbitPro-jiuchuan-OTA.bin

# Merged bin (full flash image)
python3 -m esptool --chip esp32s3 merge_bin \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 16MB \
  -o Globy-RabbitPro-jiuchuan.bin \
  0x0       build_jiuchuan/bootloader/bootloader.bin \
  0x8000    build_jiuchuan/partition_table/partition-table.bin \
  0xd000    build_jiuchuan/ota_data_initial.bin \
  0x20000   build_jiuchuan/xiaozhi_vn.bin \
  0x800000  main/Pro-Snoopy-Hitelly.bin
```

## 3) Build theo config `luxiaoban`

### 3.1 Build app

```bash
cd /Users/tamtran/Downloads/xiaozhi-esp32_vietnam-develop_vn-2
source ~/esp/esp-idf/export.sh

# Build riêng vào thư mục build_luxiaoban
idf.py -B build_luxiaoban \
  -D SDKCONFIG=sdkconfig.luxiaoban \
  build
```

### 3.2 Tạo OTA bin và merged bin

```bash
cd /Users/tamtran/Downloads/xiaozhi-esp32_vietnam-develop_vn-2

# OTA bin (chỉ app)
cp build_luxiaoban/xiaozhi_vn.bin Globy-TigerPlus-luxiaoban-OTA.bin

# Merged bin (full flash image)
python3 -m esptool --chip esp32s3 merge_bin \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 16MB \
  -o Globy-TigerPlus-luxiaoban-merged.bin \
  0x0       build_luxiaoban/bootloader/bootloader.bin \
  0x8000    build_luxiaoban/partition_table/partition-table.bin \
  0xd000    build_luxiaoban/ota_data_initial.bin \
  0x20000   build_luxiaoban/xiaozhi_vn.bin \
  0x800000  main/Plus-Snoopy-HiLily.bin
```

## 4) Kiểm tra file output

```bash
ls -lh Globy-RabbitPro-jiuchuan-OTA.bin Globy-RabbitPro-jiuchuan.bin
ls -lh Globy-TigerPlus-luxiaoban-OTA.bin Globy-TigerPlus-luxiaoban-merged.bin
```

## 5) (Tuỳ chọn) Flash trực tiếp từ thư mục build

Ví dụ Jiuchuan:

```bash
idf.py -B build_jiuchuan -p /dev/tty.usbmodemXXXX flash monitor
```

Ví dụ Luxiaoban:

```bash
idf.py -B build_luxiaoban -p /dev/tty.usbmodemXXXX flash monitor
```

## 6) Lỗi thường gặp

- `idf.py: command not found`
  - Chưa `source ~/esp/esp-idf/export.sh`.
- `No module named esptool`
  - Dùng Python trong môi trường IDF, hoặc chạy lại `source .../export.sh`.
- Thiếu file asset khi `merge_bin`
  - Jiuchuan: cần `main/Pro-Snoopy-Hitelly.bin`.
  - Luxiaoban: cần `main/Plus-Snoopy-HiLily.bin`.
- Build lỗi do cache config cũ
  - Chạy lại với clean:

```bash
idf.py -B build_jiuchuan fullclean
idf.py -B build_luxiaoban fullclean
```


## 7) Release theo version (tạo source + firmware)

Repo đã có thêm script `release_vn.sh` để chạy 1 lệnh tạo:
- Source package: `releases/<Board>-<Version>-source.zip`
- OTA bin: `releases/<Board>-<Version>-OTA.bin`
- Full merged bin: `releases/<Board>-<Version>.bin`

### 7.1 Jiuchuan theo version (ví dụ `Jiuchuan-2.1.4`)

```bash
cd /Users/tamtran/Downloads/xiaozhi-esp32_vietnam-develop_vn-2
source ~/esp/esp-idf/export.sh
./release_vn.sh jiuchuan 2.1.4
```

Nếu cần ép driver màn hình mặc định cho Jiuchuan:

```bash
# driver=2: màn mới JD9853 (mặc định)
./release_vn.sh jiuchuan 2.1.4 2

# driver=1: màn cũ GC9301
./release_vn.sh jiuchuan 2.1.4 1
```

### 7.2 Luxiaoban theo version (ví dụ `Luxiaoban-2.1.0`)

```bash
cd /Users/tamtran/Downloads/xiaozhi-esp32_vietnam-develop_vn-2
source ~/esp/esp-idf/export.sh
./release_vn.sh luxiaoban 2.1.0
```

### 7.3 Kết quả file sau release

```bash
ls -lh releases/Jiuchuan-2.1.4* 
ls -lh releases/Luxiaoban-2.1.0*
```
