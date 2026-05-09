#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <mosquitto.h>
#include <systemd/sd-daemon.h>
#include "shared_data.h"

#define MQTT_HOST       "broker.hivemq.com"
#define MQTT_PORT       1883
#define MQTT_PUB_TOPIC  "lytn_04/status"
#define MQTT_SUB_TOPIC  "lytn_04/control"
#define PUBLISH_INTERVAL_MS  5000   // Publish mỗi 5 giây

struct FireAlarmData *shared_data;

/* ── CALLBACK: Nhận lệnh từ MQTT ──────────────────────────────────────── */
void on_message(struct mosquitto *mosq, void *userdata,
                const struct mosquitto_message *message) {
    (void)mosq; (void)userdata;

    char payload[256];
    memset(payload, 0, sizeof(payload));
    int copy_len = message->payloadlen < (int)(sizeof(payload) - 1)
                   ? message->payloadlen : (int)(sizeof(payload) - 1);
    strncpy(payload, (char *)message->payload, copy_len);
    printf("\n[MQTT Nhận Lệnh]: %s\n", payload);

    float new_temp = -1.0f;
    int   new_adc  = -1;

    char *p;
    if ((p = strstr(payload, "\"th_temp\":")) != NULL)
        sscanf(p, "\"th_temp\": %f", &new_temp);
    if ((p = strstr(payload, "\"th_adc\":")) != NULL)
        sscanf(p, "\"th_adc\": %d", &new_adc);

    if (new_temp > 0 || new_adc > 0) {
        sem_wait(&shared_data->mutex);

        if (new_temp > 0) {
            shared_data->thresh_temp_high = new_temp;
            shared_data->thresh_temp_safe = new_temp - 5.0f;
        }
        if (new_adc > 0) {
            shared_data->thresh_adc_fire = new_adc;
            shared_data->thresh_adc_safe = new_adc + 500;
        }

        printf("[Đã cập nhật] Temp: %.1fC (safe: %.1fC) | ADC: %d (safe: %d)\n",
               shared_data->thresh_temp_high, shared_data->thresh_temp_safe,
               shared_data->thresh_adc_fire,  shared_data->thresh_adc_safe);

        sem_post(&shared_data->mutex);
    }
}

void on_connect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)userdata;
    if (rc == 0) {
        printf("[MQTT] Kết nối thành công!\n");
        mosquitto_subscribe(mosq, NULL, MQTT_SUB_TOPIC, 0);
    } else {
        printf("[MQTT] Kết nối thất bại, rc=%d\n", rc);
    }
}

/* ── MAIN ──────────────────────────────────────────────────────────────── */
int main() {
    // 1. Kết nối Shared Memory
    int shmid = shmget(SHM_KEY, sizeof(struct FireAlarmData), 0666 | IPC_CREAT);
    if (shmid < 0) { perror("shmget"); exit(1); }

    shared_data = (struct FireAlarmData *) shmat(shmid, NULL, 0);
    if (shared_data == (void *)-1) { perror("shmat"); exit(1); }

    // 2. Khởi tạo mosquitto
    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new("BeagleBone_Gateway", true, NULL);
    if (!mosq) { perror("mosquitto_new"); exit(1); }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);

    // 3. Kết nối broker
    printf("[MQTT] Đang kết nối %s:%d...\n", MQTT_HOST, MQTT_PORT);
    if (mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS) {
        perror("mosquitto_connect");
        exit(1);
    }

    sd_notify(0, "READY=1");

    char pub_payload[256];
    int  loop_cnt = 0;
    // Mỗi tick = 100ms, publish mỗi PUBLISH_INTERVAL_MS/100 tick
    const int PUBLISH_TICKS = PUBLISH_INTERVAL_MS / 100;

    while (1) {
        // Xử lý network (nhận subscribe, keepalive...) — 100ms mỗi lần
        int rc = mosquitto_loop(mosq, 100, 1);

        // Nếu mất kết nối → reconnect với backoff tránh CPU spin
        if (rc != MOSQ_ERR_SUCCESS) {
            printf("[MQTT] Mất kết nối (%d), đang reconnect...\n", rc);
            mosquitto_reconnect(mosq);
            usleep(3000000); // Chờ 3s trước khi thử lại, tránh busy-loop
        }

        // Publish mỗi PUBLISH_INTERVAL_MS
        if (++loop_cnt >= PUBLISH_TICKS) {
            loop_cnt = 0;

            sem_wait(&shared_data->mutex);
            float temp    = shared_data->temperature;
            int   adc     = shared_data->ir_flame_adc;
            float th_temp = shared_data->thresh_temp_high;
            int   th_adc  = shared_data->thresh_adc_fire;
            sem_post(&shared_data->mutex);

            int is_fire = (temp > th_temp || adc < th_adc) ? 1 : 0;

            snprintf(pub_payload, sizeof(pub_payload),
                     "{\"temp\": %.1f, \"adc\": %d, "
                     "\"cur_th_temp\": %.1f, \"cur_th_adc\": %d, "
                     "\"fire\": %d}",
                     temp, adc, th_temp, th_adc, is_fire);

            mosquitto_publish(mosq, NULL, MQTT_PUB_TOPIC,
                              strlen(pub_payload), pub_payload, 0, false);

            printf("[Uplink]: %s\n", pub_payload);

            // Kick WDT sau mỗi lần publish
            sd_notify(0, "WATCHDOG=1");
        }
    }

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    shmdt(shared_data);
    return 0;
}