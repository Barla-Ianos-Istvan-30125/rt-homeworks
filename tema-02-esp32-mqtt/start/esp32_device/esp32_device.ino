// Device-ul IoT din tema 01, de data aceasta pe un ESP32-C3 Super Mini.
//
// Aceeași logică și același protocol ca device-simulator din tema 01, cu trei
// taskuri FreeRTOS în loc de trei thread-uri Java:
//
//   task periodic --(coada de alarme)--> task sporadic
//        |                                     |
//        +---- telemetry ---> MQTT <--- alarm -+
//                              |
//                          commands --> command_received() --(coada de comenzi)--> task aperiodic ---> status
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

=
float read_temperature(void) {
    return temperatureRead();
}

// ---------------------------------------------------------------------------
// Stare partajată între taskuri
// ---------------------------------------------------------------------------

// Evenimentul de alarmă
typedef struct {
    float     temperature;
    float     threshold;
    long long timestamp;   // epoch ms, pentru mesajul publicat
    uint32_t  arrivalMs;   // millis(), timp monoton, pentru intervalul minim
} AlarmEvent;

// Comanda: conține textul însuși, nu un pointer.
typedef struct {
    char text[COMMAND_MAX_LEN];
} Command;

static QueueHandle_t alarmQueue   = NULL;
static QueueHandle_t commandQueue = NULL;

// Citite de taskul periodic, modificate de taskul aperiodic.
static volatile uint32_t periodMs  = PERIOD_MS_INITIAL;
static volatile float    threshold = THRESHOLD_INITIAL;

// ---------------------------------------------------------------------------
// TODO 1: TASK PERIODIC
// ---------------------------------------------------------------------------
void periodic_task(void* arg) {
    TickType_t last = xTaskGetTickCount();   // r_0
    char msg[96];

    for (;;) {
        float     temp = read_temperature();
        long long ts   = (long long)epoch_ms();
        float     thr  = threshold;          // copie locală, aceeași în tot jobul

        snprintf(msg, sizeof(msg), "%s;%lld;%.2f", DEVICE_ID, ts, temp);
        mqtt_publish("telemetry", msg);
        Serial.printf("[periodic] job: T=%.2f prag=%.2f perioada=%lu ms\n",
                      temp, thr, (unsigned long)periodMs);

        if (temp > thr) {
            AlarmEvent ev = { temp, thr, ts, (uint32_t)millis() };
            if (xQueueSend(alarmQueue, &ev, 0) != pdTRUE) {   // timeout 0
                Serial.println("[periodic] coada de alarme plina, eveniment pierdut");
            }
        }

        // Așteptare absolută: următoarea eliberare la last + T (last se actualizează).
        xTaskDelayUntil(&last, pdMS_TO_TICKS(periodMs));
    }
}

// ---------------------------------------------------------------------------
// TODO 2: TASK SPORADIC
// ---------------------------------------------------------------------------
void sporadic_task(void* arg) {
    AlarmEvent ev;
    bool       hasLast      = false;
    uint32_t   lastAccepted = 0;
    char       msg[112];

    for (;;) {
        xQueueReceive(alarmQueue, &ev, portMAX_DELAY);

        uint32_t since = ev.arrivalMs - lastAccepted;
        if (hasLast && since < MIN_INTERARRIVAL_MS) {
            Serial.printf("[sporadic] alarma respinsa: %lu ms de la ultima (minim %lu ms)\n",
                          (unsigned long)since, (unsigned long)MIN_INTERARRIVAL_MS);
            continue;
        }
        hasLast      = true;
        lastAccepted = ev.arrivalMs;

        snprintf(msg, sizeof(msg), "%s;%lld;%.2f;%.2f",
                 DEVICE_ID, ev.timestamp, ev.temperature, ev.threshold);
        mqtt_publish("alarm", msg);
        Serial.printf("[sporadic] alarma publicata: T=%.2f > prag=%.2f\n",
                      ev.temperature, ev.threshold);
    }
}

// ---------------------------------------------------------------------------
// TODO 3: TASK APERIODIC
// ---------------------------------------------------------------------------
static void publish_status(const char* body) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s;%lld;%s", DEVICE_ID, (long long)epoch_ms(), body);
    mqtt_publish("status", msg);
    Serial.printf("[aperiodic] %s\n", body);
}

void aperiodic_task(void* arg) {
    Command c;
    char name[24], param[32], body[128];

    for (;;) {
        xQueueReceive(commandQueue, &c, portMAX_DELAY);

        name[0] = param[0] = '\0';
        int n = sscanf(c.text, "%23s %31s", name, param);

        if (n >= 1 && strcmp(name, "SET_INTERVAL") == 0) {
            char* end;
            long v = strtol(param, &end, 10);
            if (n == 2 && *end == '\0' && v > 0) {
                periodMs = (uint32_t)v;
                snprintf(body, sizeof(body), "OK SET_INTERVAL %ld", v);
            } else {
                snprintf(body, sizeof(body), "ERROR interval invalid: %s", c.text);
            }

        } else if (n >= 1 && strcmp(name, "SET_THRESHOLD") == 0) {
            char* end;
            float v = strtof(param, &end);
            if (n == 2 && *end == '\0') {
                threshold = v;
                snprintf(body, sizeof(body), "OK SET_THRESHOLD %.2f", v);
            } else {
                snprintf(body, sizeof(body), "ERROR prag invalid: %s", c.text);
            }

        } else if (n == 1 && strcmp(name, "RESET") == 0) {
            periodMs  = PERIOD_MS_INITIAL;
            threshold = THRESHOLD_INITIAL;
            snprintf(body, sizeof(body), "OK RESET");

        } else if (n == 1 && strcmp(name, "STATUS") == 0) {
            snprintf(body, sizeof(body), "OK STATUS period=%lu threshold=%.2f",
                     (unsigned long)periodMs, threshold);

        } else {
            snprintf(body, sizeof(body), "ERROR comanda necunoscuta: %s", c.text);
        }

        publish_status(body);   // comanda greșită -> ERROR, taskul continuă
    }
}

// Rulează pe taskul clientului MQTT
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);

    Command c;
    strncpy(c.text, text, COMMAND_MAX_LEN - 1);
    c.text[COMMAND_MAX_LEN - 1] = '\0';
    if (xQueueSend(commandQueue, &c, 0) != pdTRUE) {
        Serial.println("[mqtt] coada de comenzi plina, comanda pierduta");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): cozile, înainte de mqtt_start()
    alarmQueue   = xQueueCreate(10, sizeof(AlarmEvent));
    commandQueue = xQueueCreate(10, sizeof(Command));
    if (alarmQueue == NULL || commandQueue == NULL) {
        Serial.println("EROARE: nu s-au putut crea cozile");
    }

    mqtt_start();

    // TODO 0 (b): taskurile, după mqtt_start()
    xTaskCreate(periodic_task,  "periodic",  4096, NULL, 5, NULL);
    xTaskCreate(sporadic_task,  "sporadic",  4096, NULL, 4, NULL);
    xTaskCreate(aperiodic_task, "aperiodic", 4096, NULL, 3, NULL);
}

void loop() {
    delay(1000);
}