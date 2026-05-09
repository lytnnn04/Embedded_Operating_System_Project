#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>           
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sched.h>
#include <systemd/sd-daemon.h>  
#include "shared_data.h"

#define WDT_KICK_INTERVAL_S  5

void set_priority() {
    struct sched_param param;
    param.sched_priority = 99;

    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        perror("Can't set Priority.\n");
    } else {
        printf("Priority 99 assigned\n");
    }
}

int main() {
    set_priority();
    int shmid;
    struct FireAlarmData *shared_data;

    shmid = shmget(SHM_KEY, sizeof(struct FireAlarmData), 0666 | IPC_CREAT);
    if (shmid < 0) {
        perror("Can't connect Shared Memory");
        exit(1);
    }

    shared_data = (struct FireAlarmData *) shmat(shmid, NULL, 0);
    if (shared_data == (struct FireAlarmData *) -1) {
        perror("Can't attach Shared Memory");
        exit(1);
    }

    int fd_alarm = open("/dev/led", O_WRONLY);
    if (fd_alarm < 0) {
        perror("Can't open LED driver.");
        exit(1);
    }

    sd_notify(0, "READY=1");

    printf("Waiting for fire sema...\n");

    while (1) {
        while (1) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += WDT_KICK_INTERVAL_S;

            int ret = sem_timedwait(&shared_data->fire, &ts);
            if (ret == 0) {
                // Có tín hiệu lửa → thoát vòng chờ, xử lý báo động
                break;
            }
            // Hết timeout (ETIMEDOUT) hoặc lỗi khác → kick WDT và chờ tiếp
            sd_notify(0, "WATCHDOG=1");
            printf("[WDT] Alarm alive, still waiting for fire...\n");
        }

        printf("Fire semaphore received\n");

        if (fd_alarm >= 0) write(fd_alarm, "1", 1); // Bật phần cứng (Còi/Đèn)

        while (1) {
            // --- BẮT ĐẦU VÙNG KHÓA (MUTEX) ---
            sem_wait(&shared_data->mutex);
            float current_temp = shared_data->temperature;
            int current_adc    = shared_data->ir_flame_adc;
            
            // Lấy 2 ngưỡng AN TOÀN (Dùng để quyết định lúc nào được Tắt còi)
            float safe_temp_th = shared_data->thresh_temp_safe;
            int safe_adc_th    = shared_data->thresh_adc_safe;
            sem_post(&shared_data->mutex);
            // --- KẾT THÚC VÙNG KHÓA ---

            // [WDT] Chứng minh vòng giám sát vẫn đang chạy, không bị kẹt
            sd_notify(0, "WATCHDOG=1");

            // So sánh dữ liệu thực tế với ngưỡng lấy từ Shared Memory
            if (current_temp < safe_temp_th && current_adc > safe_adc_th) {
                printf("Return to sleep (Temp: %.1f, ADC: %d)\n", current_temp, current_adc);
                
                if (fd_alarm >= 0) write(fd_alarm, "0", 1); // Tắt phần cứng

                // Dọn dẹp cờ cháy 
                while (sem_trywait(&shared_data->fire) == 0);
                 
                break; // Thoát vòng lặp, về trạng thái ngủ đông chờ báo cháy
            }

            usleep(200000); // Check lại sau mỗi 200ms
        }
    }

    close(fd_alarm);
    shmdt(shared_data);
    return 0;
}