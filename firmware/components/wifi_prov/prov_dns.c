#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "prov_ap.h"

static const char *TAG = "prov.dns";

#define DNS_PORT 53
#define DNS_MAX_LEN 512
#define DNS_TTL 10 /* seconds: nothing here should be cached */
#define DNS_RECV_MS 500

static TaskHandle_t s_task;
static int s_sock = -1;
static volatile bool s_run;

/* Answers every A query with our own address, whatever was asked. A
 * phone probing its connectivity URL therefore lands on the portal and
 * shows "sign in to network" instead of "no internet". */
static int build_reply(const uint8_t *q, int q_len, uint8_t *out, int out_max)
{
    if (q_len < 12 || q_len > out_max) {
        return 0;
    }
    /* Not a standard query, or no questions: ignore rather than guess. */
    if ((q[2] & 0x80) != 0 || q[4] != 0 || q[5] == 0) {
        return 0;
    }

    memcpy(out, q, q_len);
    out[2] = 0x81; /* response, recursion desired echoed */
    out[3] = 0x80; /* recursion available, no error */
    out[6] = 0;
    out[7] = 1; /* one answer */
    out[8] = 0;
    out[9] = 0; /* no authority */
    out[10] = 0;
    out[11] = 0; /* no additional */

    int len = q_len;
    if (len + 16 > out_max) {
        return 0;
    }

    /* Pointer back to the question's name, then A / IN. */
    out[len++] = 0xC0;
    out[len++] = 0x0C;
    out[len++] = 0x00;
    out[len++] = 0x01;
    out[len++] = 0x00;
    out[len++] = 0x01;
    out[len++] = 0x00;
    out[len++] = 0x00;
    out[len++] = 0x00;
    out[len++] = DNS_TTL;
    out[len++] = 0x00;
    out[len++] = 0x04;

    uint32_t addr = ipaddr_addr(PROV_AP_IP);
    memcpy(&out[len], &addr, 4);
    len += 4;
    return len;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t rx[DNS_MAX_LEN];
    uint8_t tx[DNS_MAX_LEN + 16];

    while (s_run) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue; /* timeout, or the socket was closed to stop us */
        }
        int reply = build_reply(rx, n, tx, sizeof(tx));
        if (reply > 0) {
            sendto(s_sock, tx, reply, 0, (struct sockaddr *)&from, from_len);
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t prov_dns_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    /* A receive timeout is what lets the task notice s_run going false
     * and exit, rather than blocking in recvfrom forever. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = DNS_RECV_MS * 1000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_run = true;
    if (xTaskCreate(dns_task, "prov_dns", 3072, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "captive portal DNS answering for %s", PROV_AP_IP);
    return ESP_OK;
}

void prov_dns_stop(void)
{
    if (s_task == NULL) {
        return;
    }
    s_run = false;
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    /* The task wakes on its receive timeout and deletes itself. */
    for (int i = 0; i < 20 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "DNS responder stopped");
}
