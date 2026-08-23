#!/usr/bin/env python3
import os
import sys
import glob
import subprocess

def clear_screen():
    os.system('cls' if os.name == 'nt' else 'clear')

def print_header(title):
    print("=" * 60)
    print(f" {title:^58s}")
    print("=" * 60)

def get_serial_ports():
    ports = []
    if sys.platform.startswith('darwin'):
        ports = glob.glob('/dev/cu.usb*') + glob.glob('/dev/cu.ttyUSB*')
    elif sys.platform.startswith('linux'):
        ports = glob.glob('/dev/ttyUSB*') + glob.glob('/dev/ttyACM*')
    elif sys.platform.startswith('win'):
        import serial.tools.list_ports
        ports = [p.device for p in serial.tools.list_ports.comports()]
    
    # Filter out common Bluetooth ports on Mac
    filtered = []
    for p in ports:
        if any(x in p.lower() for x in ['bluetooth', 'incoming', 'soundbar', 'marshmallow']):
            continue
        filtered.append(p)
    return filtered

def scan_files(directory, pattern):
    files = glob.glob(os.path.join(directory, pattern))
    # Sort files naturally (reverse so newest versions are at the top)
    files.sort(key=lambda x: os.path.basename(x), reverse=True)
    return files

def main():
    clear_screen()
    print_header("GLOBY SPEAKER FLASH ASSISTANT (OTA / BASE MODE)")
    
    # 1. Scan Ports
    ports = get_serial_ports()
    selected_port = None
    
    if not ports:
        print("[-] Warning: No USB serial ports detected.")
        port_input = input("Please plug in the speaker or enter port path manually: ").strip()
        if port_input:
            selected_port = port_input
        else:
            print("[-] Exiting.")
            sys.exit(1)
    elif len(ports) == 1:
        selected_port = ports[0]
        print(f"[+] Autodetected port: {selected_port}")
    else:
        print("Multiple serial ports found:")
        for idx, port in enumerate(ports):
            print(f"  [{idx + 1}] {port}")
        while True:
            try:
                choice = int(input(f"Select port (1-{len(ports)}): "))
                if 1 <= choice <= len(ports):
                    selected_port = ports[choice - 1]
                    break
            except ValueError:
                pass
            print("Invalid choice, please try again.")
            
    print(f"[+] Selected Port: {selected_port}\n")
    
    # 2. Scan OTA App Files in Apps-OTA/
    ota_dir = "Apps-OTA"
    ota_files = scan_files(ota_dir, "*.bin")
    
    v1_ota_files = [f for f in ota_files if "V2" not in os.path.basename(f)]
    v2_ota_files = [f for f in ota_files if "V2" in os.path.basename(f)]
    
    # 3. Scan Assets in assets_bin/
    assets_dir = "assets_bin"
    asset_files = scan_files(assets_dir, "*.bin")
    
    # 4. Interactive App Selection
    print_header("SELECT APP (OTA BINARY)")
    print("  [1] Loa Pro V1 (RabbitPro)")
    if v1_ota_files:
        print(f"      (Latest: {os.path.basename(v1_ota_files[0])})")
    print("  [2] Loa Pro V2 (RabbitPro V2)")
    if v2_ota_files:
        print(f"      (Latest: {os.path.basename(v2_ota_files[0])})")
    print("  [3] Choose another OTA firmware file")
    print("  [4] Do Not Flash App (Skip App)")
    
    selected_app = None
    while True:
        try:
            choice = int(input("Enter choice (1-4): "))
            if choice == 1:
                if not v1_ota_files:
                    print("[-] No V1 OTA binaries found in Apps-OTA/ folder.")
                    continue
                if len(v1_ota_files) == 1:
                    selected_app = v1_ota_files[0]
                else:
                    print("\nSelect V1 version:")
                    for idx, f in enumerate(v1_ota_files):
                        print(f"  [{idx + 1}] {os.path.basename(f)}")
                    c = int(input(f"Select version (1-{len(v1_ota_files)}): "))
                    selected_app = v1_ota_files[c - 1]
                break
            elif choice == 2:
                if not v2_ota_files:
                    print("[-] No V2 OTA binaries found in Apps-OTA/ folder.")
                    continue
                if len(v2_ota_files) == 1:
                    selected_app = v2_ota_files[0]
                else:
                    print("\nSelect V2 version:")
                    for idx, f in enumerate(v2_ota_files):
                        print(f"  [{idx + 1}] {os.path.basename(f)}")
                    c = int(input(f"Select version (1-{len(v2_ota_files)}): "))
                    selected_app = v2_ota_files[c - 1]
                break
            elif choice == 3:
                path = input("Enter path to OTA firmware .bin file: ").strip()
                if os.path.exists(path):
                    selected_app = path
                    break
                else:
                    print("[-] File not found.")
            elif choice == 4:
                selected_app = None
                break
        except (ValueError, IndexError):
            pass
        print("Invalid choice, please try again.")

    print(f"[+] App selected: {os.path.basename(selected_app) if selected_app else 'None'}\n")

    # 5. Asset selection
    print_header("SELECT ASSET COLOR")
    selected_asset = None
    if not asset_files:
        print(f"[-] No assets found in '{assets_dir}' folder.")
        print("  [1] Do Not Flash Assets")
    else:
        for idx, f in enumerate(asset_files):
            name = os.path.basename(f)
            color_part = name.replace("Assets-", "").split("-")[0]
            print(f"  [{idx + 1}] Màu {color_part} ({name})")
        print(f"  [{len(asset_files) + 1}] Do Not Flash Assets")
        
        while True:
            try:
                c = int(input(f"Select asset (1-{len(asset_files) + 1}): "))
                if 1 <= c <= len(asset_files):
                    selected_asset = asset_files[c - 1]
                    break
                elif c == len(asset_files) + 1:
                    selected_asset = None
                    break
            except ValueError:
                pass
            print("Invalid choice, please try again.")
            
    print(f"[+] Asset selected: {os.path.basename(selected_asset) if selected_asset else 'None'}\n")

    # 6. Mode selection
    if not selected_app and not selected_asset:
        print("[!] Nothing selected to flash. Exiting.")
        sys.exit(0)
        
    print_header("SELECT FLASH MODE")
    available_modes = []
    
    if selected_app and selected_asset:
        print("  [1] Nạp Full (Bootloader + Partition + OTA Data + App + Assets) - Dành cho loa mới/lỗi")
        print("  [2] Nạp nhanh cả App + Assets (Chỉ nạp 2 thành phần này)")
        print("  [3] Chỉ nạp App")
        print("  [4] Chỉ nạp Assets")
        available_modes = [1, 2, 3, 4]
    elif selected_app:
        print("  [1] Nạp Full App (Bootloader + Partition + OTA Data + App) - Dành cho loa mới/lỗi")
        print("  [2] Chỉ nạp App (Nạp nhanh)")
        available_modes = [1, 2]
    elif selected_asset:
        print("  [1] Nạp Assets (Nạp nhanh vào 0x800000)")
        available_modes = [1]
        
    selected_mode = 1
    if len(available_modes) > 1:
        while True:
            try:
                selected_mode = int(input(f"Select flash mode (1-{len(available_modes)}): "))
                if selected_mode in available_modes:
                    break
            except ValueError:
                pass
            print("Invalid choice, please try again.")

    # 7. Confirm flashing
    print_header("CONFIRM FLASH DETAILS")
    print(f"  Port      : {selected_port}")
    print(f"  App       : {os.path.basename(selected_app) if selected_app else 'SKIP'}")
    print(f"  Asset     : {os.path.basename(selected_asset) if selected_asset else 'SKIP'}")
    
    # Determine the flash command components based on selection and mode
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", selected_port,
        "--baud", "921600",
        "write_flash"
    ]
    
    base_dir = "base_bin"
    is_full_flash = False
    
    if selected_app and selected_asset:
        if selected_mode == 1:
            is_full_flash = True
            cmd.extend([
                "0x0", os.path.join(base_dir, "bootloader.bin"),
                "0x8000", os.path.join(base_dir, "partition-table.bin"),
                "0x11000", os.path.join(base_dir, "ota_data_initial.bin"),
                "0x20000", selected_app,
                "0x800000", selected_asset
            ])
        elif selected_mode == 2:
            cmd.extend(["0x20000", selected_app, "0x800000", selected_asset])
        elif selected_mode == 3:
            cmd.extend(["0x20000", selected_app])
        elif selected_mode == 4:
            cmd.extend(["0x800000", selected_asset])
            
    elif selected_app:
        if selected_mode == 1:
            is_full_flash = True
            cmd.extend([
                "0x0", os.path.join(base_dir, "bootloader.bin"),
                "0x8000", os.path.join(base_dir, "partition-table.bin"),
                "0x11000", os.path.join(base_dir, "ota_data_initial.bin"),
                "0x20000", selected_app
            ])
        elif selected_mode == 2:
            cmd.extend(["0x20000", selected_app])
            
    elif selected_asset:
        cmd.extend(["0x800000", selected_asset])
        
    print(f"  Mode      : {'FULL FLASH (App + Base Systems)' if is_full_flash else 'MODULAR FLASH (Fast)'}")
    print("-" * 60)
    
    confirm = input("Are you sure you want to write flash? (y/N): ").strip().lower()
    if confirm != 'y':
        print("[!] Canceled by user.")
        sys.exit(0)
        
    print(f"\n[+] Executing: {' '.join(cmd)}")
    
    try:
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in process.stdout:
            print(line, end="")
        process.wait()
        
        if process.returncode == 0:
            print("\n[+] Success! Flashing completed successfully.")
        else:
            print(f"\n[-] Error: Flashing failed with exit code {process.returncode}")
    except Exception as e:
        print(f"\n[-] Error running esptool: {e}")

if __name__ == '__main__':
    main()
