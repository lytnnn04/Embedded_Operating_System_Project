# Bài tập lớn kết thúc học phần - Hệ Điều Hành Nhúng

Thiết kế và xây dựng một hệ thống báo cháy nhúng chạy trên **BeagleBone Black (AM335x)** với Linux, bao gồm các kernel driver tự viết và ứng dụng user-space đa tiến trình giao tiếp qua Shared Memory.

---

## Kiến Trúc Tổng Quan

```
┌─────────────────────────────────────────────────────────────────┐
│                        User-space                               │
│                                                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────┐  ┌────────┐    │
│  │ sensor_node  │  │    alarm     │  │ lcd_ui  │  │  mqtt  │    │
│  │ (FIFO Pr.90) │  │ (FIFO Pr.99) │  │         │  │        │    │
│  └──────┬───────┘  └──────┬───────┘  └────┬────┘  └───┬────┘    │
│         │                 │               │           │         │
│         └─────────────────┴───────────────┴───────────┘         │
│                    POSIX Shared Memory (key=0x1234)             │
│                    Semaphore: mutex + fire                      │
└─────────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────────┐
│                        Kernel-space                             │
│  /dev/adc_device   /dev/dht22   /dev/led   /dev/lcd_i2c         │
│  (ADC Driver)      (DHT22 Drv)  (LED Drv)  (I2C LCD Drv)        │
└─────────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────────┐
│                        Hardware                                 │
│  IR Flame Sensor   DHT22 Sensor  LED/Buzzer  LCD 16x2 I2C       │
│  (AIN0 - ADC)      (GPIO1_16)    (GPIO1_28)  (I2C2, addr 0x27)  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Cấu Trúc Thư Mục

```
btl/
├── app/                        # Ứng dụng user-space
│   ├── shared_data.h           # Định nghĩa cấu trúc Shared Memory dùng chung
│   ├── sensor_node.c           # Tiến trình đọc cảm biến & phát hiện cháy
│   ├── alarm.c                 # Tiến trình điều khiển còi/đèn báo động
│   ├── lcd_ui.c                # Tiến trình hiển thị giao diện LCD
│   └── mqtt.c                  # Tiến trình gateway MQTT (IoT)
├── drv/                        # Linux Kernel Drivers
│   ├── adc_driver.c            # Character driver cho cảm biến lửa IR (ADC)
│   ├── dht.c                   # Character driver cho cảm biến nhiệt độ/độ ẩm DHT22
│   ├── led.c                   # Character driver điều khiển LED/Còi báo động
│   └── i2cdrv.c                # Character driver I2C cho màn hình LCD 16x2
├──service/
|   ├── fire_alarm.service      # Dịch vụ quản lý fire_alarm
│   ├── fire_lcd.service        # Dịch vụ quản lý fire_lcd
│   ├── fire_sensor.service     # Dịch vụ quản lý fire_sensor
|   ├── load-drivers.service    # Dịch vụ khởi động load drv      
│   └── fire_mqtt.service       # Dịch vụ khởi động fire_mqtt    
└── exe/                        
    ├── fire_sensor             # Thực thi từ sensor_node.c
    ├── fire_alarm              # Thực thi từ alarm.c
    ├── fire_lcd                # Thực thi từ lcd_ui.c
    └── fire_mqtt               # Thực thi từ mqtt.c
```

---

## Kernel Drivers (`drv/`)

### 1. `adc_driver.c` — Driver Cảm Biến Lửa IR
- **Device node**: `/dev/adc_device`
- Truy cập trực tiếp thanh ghi phần cứng ADC của AM335x (base `0x44E0D000`)
- Đọc kênh **AIN0** trả về giá trị 12-bit (0–4095); giá trị **thấp** = phát hiện lửa
- Bật xung clock ADC qua `CM_WKUP` (`0x44E00400`)

### 2. `dht.c` — Driver Cảm Biến Nhiệt Độ & Độ Ẩm DHT22
- **Device node**: `/dev/dht22`
- Điều khiển giao tiếp 1-wire với DHT22 qua **GPIO1_16 (P9_15)**
- Trả về chuỗi dạng `"nhiệt_độ , độ_ẩm"` (ví dụ: `"32.5 , 65.0"`)
- Hỗ trợ đầy đủ 4 module GPIO của AM335x (GPIO0–GPIO3)
- Sử dụng mutex kernel để bảo vệ truy cập đồng thời

### 3. `led.c` — Driver LED / Còi Báo Động
- **Device node**: `/dev/led`
- Điều khiển **GPIO1_28 (P9_12)** qua thao tác trực tiếp thanh ghi
- Ghi `'1'` để BẬT còi/đèn, ghi `'0'` để TẮT
- Đọc trạng thái hiện tại của chân GPIO

### 4. `i2cdrv.c` — Driver I2C LCD 16x2
- **Device node**: `/dev/lcd_i2c`
- Giao tiếp với module LCD 16x2 qua **I2C2** (địa chỉ slave `0x27`)
- Cấu hình chân I2C qua Control Module (`UART1_RTSN`/`CTSN`)
- Hỗ trợ các lệnh **IOCTL**:
  | Lệnh | Mô tả |
  |------|-------|
  | `LCD_IOCTL_CLEAR` | Xóa màn hình |
  | `LCD_IOCTL_HOME` | Đưa con trỏ về đầu |
  | `LCD_IOCTL_SETPOS` | Đặt vị trí con trỏ (hàng, cột) |
  | `LCD_IOCTL_INIT` | Khởi tạo lại phần cứng LCD |

---

## Ứng Dụng User-space (`app/`)

### Cấu Trúc Dữ Liệu Dùng Chung (`shared_data.h`)

```c
struct FireAlarmData {
    float temperature;       // Nhiệt độ đo được (°C)
    float humidity;          // Độ ẩm đo được (%)
    int   ir_flame_adc;      // Giá trị ADC cảm biến lửa (0–4095)

    float thresh_temp_high;  // Ngưỡng nhiệt độ báo cháy (mặc định: 50°C)
    float thresh_temp_safe;  // Ngưỡng nhiệt độ an toàn  (mặc định: 45°C)
    int   thresh_adc_fire;   // Ngưỡng ADC báo cháy      (mặc định: <1000)
    int   thresh_adc_safe;   // Ngưỡng ADC an toàn        (mặc định: >1500)

    sem_t mutex;             // Semaphore bảo vệ vùng dữ liệu dùng chung
    sem_t fire;              // Semaphore báo hiệu có lửa
};
```

### 1. `sensor_node.c` — Tiến Trình Đọc Cảm Biến
- **SCHED_FIFO, Priority 90** — ưu tiên real-time cao
- Đọc ADC mỗi **500ms**, đọc DHT22 mỗi **2 giây**
- So sánh dữ liệu với ngưỡng động từ Shared Memory
- Kích semaphore `fire` khi phát hiện cháy (ADC thấp **hoặc** nhiệt độ cao)
- Tạo và khởi tạo Shared Memory lần đầu; tự động attach vào SHM đã tồn tại khi restart
- Tích hợp **systemd Watchdog** (`sd_notify WATCHDOG=1`)

### 2. `alarm.c` — Tiến Trình Báo Động
- **SCHED_FIFO, Priority 99** — ưu tiên real-time cao nhất
- Chờ semaphore `fire` với `sem_timedwait` (timeout 5s để kick Watchdog)
- Khi có cháy: ghi `'1'` vào `/dev/led` để BẬT còi/đèn
- Tự động TẮT còi khi cả nhiệt độ và ADC trở về ngưỡng an toàn
- Tích hợp **systemd Watchdog**

### 3. `lcd_ui.c` — Tiến Trình Hiển Thị LCD
- Cập nhật màn hình LCD mỗi **1 giây** mà không xóa toàn màn hình (tối ưu hiệu suất I2C)
- **Chế độ Bình Thường**: Hiển thị nhiệt độ, độ ẩm và giá trị ADC
  ```
  T: 32.5C  H: 65%
  Flame ADC: 3820
  ```
- **Chế độ Báo Cháy**: Hiển thị cảnh báo đỏ
  ```
  CANH BAO CHAY!
  Temp: 55.2C
  ```
- Tự động kết nối lại nếu LCD bị ngắt kết nối đột ngột
- Tích hợp **systemd Watchdog**

### 4. `mqtt.c` — Tiến Trình Gateway MQTT
- Kết nối broker công cộng **HiveMQ** (`broker.hivemq.com:1883`)
- **Publish** dữ liệu lên topic `lytn_04/status` mỗi **5 giây**:
  ```json
  {"temp": 32.5, "adc": 3820, "cur_th_temp": 50.0, "cur_th_adc": 1000, "fire": 0}
  ```
- **Subscribe** topic `lytn_04/control` để nhận lệnh thay đổi ngưỡng từ xa:
  ```json
  {"th_temp": 45.0, "th_adc": 800}
  ```
- Tự động reconnect khi mất kết nối (backoff 3s)
- Tích hợp **systemd Watchdog**

---

## Tính Năng Nổi Bật

| Tính Năng | Mô Tả |
|-----------|-------|
| **Real-time** | Sử dụng `SCHED_FIFO` cho các tiến trình quan trọng |
| **IPC an toàn** | POSIX Shared Memory + Semaphore chống race condition |
| **Ngưỡng động** | Thay đổi ngưỡng báo cháy từ xa qua MQTT không cần restart |
| **Watchdog** | Tích hợp systemd Watchdog, tự động khởi động lại nếu tiến trình bị treo |
| **IoT** | Giám sát và điều khiển từ xa qua giao thức MQTT |
| **Tự phục hồi** | LCD và MQTT tự reconnect khi mất kết nối |
| **Hiệu quả** | LCD chỉ xóa màn hình khi chuyển chế độ, tránh nhấp nháy |
| **Driver tự viết** | 4 kernel module C tùy chỉnh, truy cập thẳng thanh ghi phần cứng |

---

## Luồng Hoạt Động

```
sensor_node đọc ADC + DHT22
        │
        ▼
Cập nhật Shared Memory (mutex)
        │
        ├─── ADC < ngưỡng HOẶC Nhiệt độ > ngưỡng?
        │           │ CÓ
        │           ▼
        │    sem_post(fire) ──────► alarm thức dậy → BẬT còi/đèn
        │                                    │
        │                          Theo dõi đến khi an toàn
        │                                    │
        │                          TẮT còi/đèn, về chế độ ngủ
        │
        ├─── lcd_ui đọc SHM mỗi 1s → cập nhật màn hình LCD
        │
        └─── mqtt publish mỗi 5s → HiveMQ broker
                    ▲
             Nhận lệnh điều khiển ngưỡng từ xa
```


---

## Phần Cứng Kết Nối

| Thiết Bị | Giao Tiếp | Chân BBB |
|----------|-----------|----------|
| IR Flame Sensor | ADC (AIN0) | P9_39 |
| DHT22 | GPIO (1-wire) | P9_15 (GPIO1_16) |
| LED / Buzzer | GPIO | P9_12 (GPIO1_28) |
| LCD 16x2 (PCF8574) | I2C2 | P9_21/P9_22 |
