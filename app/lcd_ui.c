#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <systemd/sd-daemon.h> 
#include "shared_data.h"

// Định nghĩa các lệnh IOCTL cho LCD
#define LCD_IOC_MAGIC     'L'
#define LCD_IOCTL_CLEAR   _IO (LCD_IOC_MAGIC, 0)

int main() {
    int shmid;
    struct FireAlarmData *shared_data;

    printf("[LCD UI] Dang khoi dong giao dien...\n");

    // 1. Ket noi Shared Memory
    shmid = shmget(SHM_KEY, sizeof(struct FireAlarmData), 0666 | IPC_CREAT);
    if (shmid < 0) {
        perror("Loi SHM");
        exit(1);
    }
    shared_data = (struct FireAlarmData *) shmat(shmid, NULL, 0);
    if (shared_data == (struct FireAlarmData *) -1) {
        perror("Loi shmat");
        exit(1);
    }

    // 2. Mo Driver LCD lan dau (neu that bai chi canh bao, se thu lai trong vong lap)
    int fd_lcd = open("/dev/lcd_i2c", O_WRONLY);
    if (fd_lcd < 0) {
        perror("Canh bao: Khong the mo /dev/lcd_i2c, se thu lai...");
    }

    // [WDT] Bao systemd da san sang (du LCD chua mo duoc)
    sd_notify(0, "READY=1");

    float temp, hum, th_temp;
    int adc, th_adc;
    int current_mode = -1; // -1: Chua khoi tao, 0: Binh thuong, 1: Bao chay

    while (1) {
        // --- BUOC 0: THU MO LAI LCD NEU DANG MAT KET NOI ---
        if (fd_lcd < 0) {
            fd_lcd = open("/dev/lcd_i2c", O_WRONLY);
            if (fd_lcd >= 0) {
                printf("[LCD] Ket noi lai thanh cong. Dang khoi tao lai...\n");
                ioctl(fd_lcd, _IO('L', 3)); // Lệnh số 3: Sốc điện phần cứng
                current_mode = -1;          // Ép vẽ lại khung nền từ đầu
            } else {
                sd_notify(0, "WATCHDOG=1");
                sleep(1);
                continue;
            }
        }

        // --- BUOC 1: DOC DU LIEU ---
        sem_wait(&shared_data->mutex);
        temp    = shared_data->temperature;
        hum     = shared_data->humidity;
        adc     = shared_data->ir_flame_adc;
        th_temp = shared_data->thresh_temp_high; 
        th_adc  = shared_data->thresh_adc_fire;
        sem_post(&shared_data->mutex);

        // --- BUOC 2: VẼ KHUNG NỀN (CHỈ XÓA MÀN HÌNH KHI CHUYỂN CHẾ ĐỘ) ---
        int new_mode = (temp > th_temp || adc < th_adc) ? 1 : 0;
        
        if (new_mode != current_mode) {
            ioctl(fd_lcd, LCD_IOCTL_CLEAR); 
            if (new_mode == 1) {
                write(fd_lcd, "CANH BAO CHAY!  \nTemp: ", 24);
            } else {
                write(fd_lcd, "T:      H:      \nFlame ADC: ", 28);
            }
            current_mode = new_mode; 
        }

        // --- BUOC 3: NHẢY CON TRỎ VÀ ĐIỀN SỐ (KHÔNG DÙNG LỆNH CLEAR NỮA) ---
        char num_buf[16];
        unsigned int pos;
        int write_err = 0;

        if (current_mode == 1) {
            // [BÁO CHÁY] - Nhảy đến Hàng 1, Cột 6
            pos = (1 << 8) | 6; 
            ioctl(fd_lcd, _IOW('L', 2, unsigned int), &pos); 
            snprintf(num_buf, sizeof(num_buf), "%.1fC  ", temp); 
            if (write(fd_lcd, num_buf, strlen(num_buf)) < 0) write_err = 1;
            
        } else {
            // [BÌNH THƯỜNG] - Điền Temp (Hàng 0, Cột 2)
            pos = (0 << 8) | 2;
            ioctl(fd_lcd, _IOW('L', 2, unsigned int), &pos);
            snprintf(num_buf, sizeof(num_buf), "%.1fC ", temp);
            if (write(fd_lcd, num_buf, strlen(num_buf)) < 0) write_err = 1;

            // Điền Hum (Hàng 0, Cột 10)
            if (!write_err) {
                pos = (0 << 8) | 10;
                ioctl(fd_lcd, _IOW('L', 2, unsigned int), &pos);
                snprintf(num_buf, sizeof(num_buf), "%.0f%% ", hum);
                if (write(fd_lcd, num_buf, strlen(num_buf)) < 0) write_err = 1;
            }

            // Điền ADC (Hàng 1, Cột 11)
            if (!write_err) {
                pos = (1 << 8) | 11;
                ioctl(fd_lcd, _IOW('L', 2, unsigned int), &pos);
                snprintf(num_buf, sizeof(num_buf), "%04d", adc);
                if (write(fd_lcd, num_buf, strlen(num_buf)) < 0) write_err = 1;
            }
        }

        // Kiểm tra xem lệnh write có bị lỗi do đứt cáp không
        if (write_err) {
            perror("[LCD] Loi ghi man hinh, dong ket noi...");
            close(fd_lcd);
            fd_lcd = -1; 
        }

        sd_notify(0, "WATCHDOG=1");
        sleep(1); 
    }

    close(fd_lcd);
    shmdt(shared_data);
    return 0;
}