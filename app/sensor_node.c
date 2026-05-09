#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sched.h>
#include <systemd/sd-daemon.h> 
#include "shared_data.h"

void set_priority() {
    struct sched_param param;
    param.sched_priority = 90; 
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        perror("Can't set priority\n");
    } else {
        printf("Priority 90 assigned!\n");
    }
}

int main() {
    set_priority();
    int shmid;
    struct FireAlarmData *shared_data;

    // Thử tạo SHM mới (IPC_EXCL đảm bảo chỉ thành công nếu chưa tồn tại)
    int is_new_shm = 0;
    shmid = shmget(SHM_KEY, sizeof(struct FireAlarmData), 0666 | IPC_CREAT | IPC_EXCL);
    if (shmid >= 0) {
        is_new_shm = 1;
    } else {
        // SHM đã tồn tại → chỉ attach, không init lại semaphore
        shmid = shmget(SHM_KEY, sizeof(struct FireAlarmData), 0666);
        if (shmid < 0) {
            perror("Can't connect Shared Memory");
            exit(1);
        }
    }

    shared_data = (struct FireAlarmData *) shmat(shmid, NULL, 0);
    if (shared_data == (struct FireAlarmData *) -1) {
        perror("Can't attach Shared Memory");
        exit(1);
    }

    if (is_new_shm) {
        // Chỉ khởi tạo semaphore và ngưỡng khi SHM được tạo lần đầu
        // Tránh reset state khi sensor_node restart
        sem_init(&shared_data->mutex, 1, 1);
        sem_init(&shared_data->fire, 1, 0);

        // --- GÁN GIÁ TRỊ NGƯỠNG MẶC ĐỊNH LÚC KHỞI ĐỘNG ---
        shared_data->thresh_temp_high = 50.0f;
        shared_data->thresh_temp_safe = 45.0f;
        shared_data->thresh_adc_fire  = 1000;
        shared_data->thresh_adc_safe  = 1500;
        printf("Shared Memory, Semaphores & Thresholds Initialized.\n");
    } else {
        printf("Attached to existing Shared Memory (semaphores preserved).\n");
    }
    
    sd_notify(0, "READY=1"); // systemd sẽ nhận tín hiệu này để kích hoạt 2 file alarm và lcd_ui chạy theo

    char buf[64]; 
    int current_adc = 4095;
    float current_temp = 25.0f;
    float current_hum = 50.0f;
    float temp_f, hum_f;
    int cnt = 0;

    while (1) {
        int fd_adc = open("/dev/adc_device", O_RDONLY);
        if (fd_adc >= 0) {
            memset(buf, 0, sizeof(buf));
            if (read(fd_adc, buf, sizeof(buf)) > 0) {
                current_adc = atoi(buf); 
            }
            close(fd_adc); 
        } else {
            printf("Không thể mở ADC!\n");
        }

        if (cnt % 4 == 0) {
            int fd_dht = open("/dev/dht22", O_RDONLY);
            if (fd_dht >= 0) {
                memset(buf, 0, sizeof(buf));
                if (read(fd_dht, buf, sizeof(buf)) > 0) {
                    if (sscanf(buf, "%f , %f", &temp_f, &hum_f) == 2) {
                        current_temp = temp_f;
                        current_hum  = hum_f;
                    }
                }
                close(fd_dht); 
            } else {
                printf("Không thể mở DHT22!\n");
            }
        }

        sem_wait(&shared_data->mutex); 
        
        // 1. Cập nhật dữ liệu cảm biến thực tế
        shared_data->ir_flame_adc = current_adc;
        shared_data->temperature = current_temp;
        shared_data->humidity = current_hum;
        
        // 2. Đọc ngưỡng BÁO CHÁY từ bộ nhớ chung để đối chiếu
        float local_thresh_temp = shared_data->thresh_temp_high;
        int local_thresh_adc    = shared_data->thresh_adc_fire;

        sem_post(&shared_data->mutex); 
        
        sd_notify(0, "WATCHDOG=1"); // Chứng minh vòng lặp vẫn đang chạy trơn tru, không bị kẹt

        // So sánh dữ liệu thực tế với ngưỡng cấu hình động
        if(current_adc < local_thresh_adc || current_temp > local_thresh_temp) {
            // sem_trywait + sem_post: atomic check-and-set (tránh TOCTOU)
            // Nếu fire đang = 0 (trywait fail/EAGAIN) → post lên 1
            // Nếu fire đang = 1 (trywait thành công = 0) → post lại = 1
            // Kết quả: semaphore luôn đúng = 1, không thể > 1
            sem_trywait(&shared_data->fire);
            sem_post(&shared_data->fire);
        }
        cnt++;
        usleep(500000); 
    }
    shmdt(shared_data);
    return 0;
}