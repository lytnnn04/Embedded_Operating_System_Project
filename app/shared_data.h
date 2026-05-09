#ifndef SHARED_DATA_H
#define SHARED_DATA_H
#include <semaphore.h>

#define SHM_KEY 0x1234 // Key của bạn

struct FireAlarmData {
    // Vùng Dữ liệu Cảm biến 
    float temperature;
    float humidity;
    int ir_flame_adc;

    // Vùng Ngưỡng Cấu Hình 
    float thresh_temp_high;  // Ngưỡng nhiệt độ báo cháy 
    float thresh_temp_safe;  // Ngưỡng nhiệt độ an toàn 
    int thresh_adc_fire;     // Ngưỡng ADC báo cháy 
    int thresh_adc_safe;     // Ngưỡng ADC an toàn 


    // 3. Cơ chế IPC
    sem_t mutex;
    sem_t fire;
};

#endif