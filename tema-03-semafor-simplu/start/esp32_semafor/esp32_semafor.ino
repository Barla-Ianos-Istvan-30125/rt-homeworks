// Semafor pentru pietoni, cu buton, pe ESP32-C3 Super Mini.
//
// Placa este controlerul semaforului; lămpile sunt desenate de dashboard, care
// le aprinde după mesajele "lights" publicate de placă. Patru activități:
//
//   ISR buton BOOT ----+
//                      +--(coada butonului)--> task sporadic
//   command_received --+   (BUTTON)                 |
//                                                   v
//                                         [ starea semaforului ] <-- mutex
//                                            ^          |
//                                            |          v
//                         task periodic de control   task periodic de ieșire
//                         (fazele, T = 100 ms)       (lămpile, T = 500 ms) --> lights
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, pinii, perioadele, duratele fazelor
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Restul îl scrieți voi: TODO 0 ... TODO 3.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Fazele semaforului (vezi tabelul și diagrama din README.md). Numele din
// PHASE_NAMES sunt cele din mesajul "lights", în aceeași ordine ca în enum.
enum Phase { CAR_GREEN, CAR_YELLOW, ALL_RED, PED_GREEN };
const char* const PHASE_NAMES[] = {"CAR_GREEN", "CAR_YELLOW", "ALL_RED", "PED_GREEN"};

// Culorile unei lămpi, cu numele din mesajul "lights".
enum Light { RED, YELLOW, GREEN };
const char* const LIGHT_NAMES[] = {"RED", "YELLOW", "GREEN"};

// TODO 0 (c): STAREA SEMAFORULUI, partajată de taskuri și protejată de un
//   mutex (xSemaphoreCreateMutex). De exemplu un struct cu:
//   - faza curentă
//   - momentul intrării în faza curentă (millis()), din care se calculează
//     cât a durat faza și cât mai are
//   - cererea pietonului (lampa AȘTEPTAȚI): setată de taskul sporadic,
//     ștearsă de taskul de control la intrarea în PED_GREEN
//   Mutexul se ține cât mai puțin: în el doar citiți sau scrieți câmpurile
//   (eventual copiați starea într-o variabilă locală). Fără mqtt_publish(),
//   Serial sau așteptări cât timp țineți mutexul.
struct TrafficState {
    Phase    phase;
    uint32_t phaseStartMs;
    bool     pedRequest;
};

static TrafficState      state;
static SemaphoreHandle_t stateMutex;

struct ButtonPress {
    TickType_t tick;
    char       source;
};

static QueueHandle_t buttonQueue;

#define CONTROL_PRIORITY 5
#define OUTPUT_PRIORITY  4
#define BUTTON_PRIORITY  3
#define TASK_STACK       4096

// TODO 1: TASK PERIODIC DE CONTROL, cu perioada CONTROL_PERIOD_MS.
//   La fiecare job, cu mutexul luat, decide dacă faza curentă s-a terminat și
//   trece în faza următoare, după tabelul din README.md:
//     CAR_GREEN  -> CAR_YELLOW  după CAR_GREEN_MIN_MS, doar dacă există o cerere
//     CAR_YELLOW -> ALL_RED     după CAR_YELLOW_MS
//     ALL_RED    -> PED_GREEN   după ALL_RED_MS; aici se șterge cererea pietonului
//     PED_GREEN  -> CAR_GREEN   după PED_GREEN_MS
//   La fiecare schimbare de fază notați momentul intrării în faza nouă.
//   Taskul nu publică nimic: doar actualizează starea. După ce eliberează
//   mutexul, afișează pe Serial faza nouă, dacă s-a schimbat.
//   Eliberările respectă grila r_k = r_0 + kT: așteptare absolută (xTaskDelayUntil).
//   Durata unei faze se măsoară cu un timp monoton (millis()), nu cu epoch_ms().
static Phase next_phase(Phase p, uint32_t elapsed, bool request) {
    switch (p) {
        case CAR_GREEN:  return (elapsed >= CAR_GREEN_MIN_MS && request) ? CAR_YELLOW : CAR_GREEN;
        case CAR_YELLOW: return (elapsed >= CAR_YELLOW_MS) ? ALL_RED   : CAR_YELLOW;
        case ALL_RED:    return (elapsed >= ALL_RED_MS)    ? PED_GREEN : ALL_RED;
        case PED_GREEN:  return (elapsed >= PED_GREEN_MS)  ? CAR_GREEN : PED_GREEN;
    }
    return CAR_GREEN;
}

void control_task(void* arg) {
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        bool  changed = false;
        Phase current;

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        uint32_t now     = millis();
        uint32_t elapsed = now - state.phaseStartMs;
        Phase    next    = next_phase(state.phase, elapsed, state.pedRequest);
        if (next != state.phase) {
            state.phase        = next;
            state.phaseStartMs = now;
            if (next == PED_GREEN) state.pedRequest = false;
            changed = true;
        }
        current = state.phase;
        xSemaphoreGive(stateMutex);

        if (changed) {
            Serial.printf("[control] %lu ms: faza %s\n", (unsigned long)now, PHASE_NAMES[current]);
        }

        xTaskDelayUntil(&last, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

// TODO 2: TASK PERIODIC DE IEȘIRE, cu perioada OUTPUT_PERIOD_MS.
//   La fiecare job:
//   - copiază starea (cu mutexul luat, cât mai scurt)
//   - stabilește culorile celor două lămpi din faza curentă (tabelul din README.md)
//   - calculează secundele rămase din verdele pietonilor: în PED_GREEN,
//     rotunjite în sus (7,2 s rămase -> 8); 0 în celelalte faze
//   - publică: mqtt_publish("lights", "deviceId;timestamp;faza;masini;pietoni;asteptati;secunde")
//     de exemplu "esp32-a1b2c3;1727600000000;PED_GREEN;RED;GREEN;0;8"
//   - aprinde LED-ul de pe placă (LED_PIN, aprins pe LOW) cât timp lampa
//     AȘTEPTAȚI este aprinsă
//   Așteptare absolută (xTaskDelayUntil), ca și la taskul de control.
void output_task(void* arg) {
    TickType_t last = xTaskGetTickCount();
    char msg[160];

    for (;;) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        TrafficState s = state;
        xSemaphoreGive(stateMutex);

        Light car, ped;
        switch (s.phase) {
            case CAR_GREEN:  car = GREEN;  ped = RED;   break;
            case CAR_YELLOW: car = YELLOW; ped = RED;   break;
            case ALL_RED:    car = RED;    ped = RED;   break;
            case PED_GREEN:  car = RED;    ped = GREEN; break;
            default:         car = RED;    ped = RED;   break;
        }

        int secs = 0;
        if (s.phase == PED_GREEN) {
            uint32_t elapsed = millis() - s.phaseStartMs;
            uint32_t remain  = (elapsed < PED_GREEN_MS) ? (PED_GREEN_MS - elapsed) : 0;
            secs = (int)((remain + 999) / 1000);
            if (secs < 1) secs = 1;
        }

        int waiting = s.pedRequest ? 1 : 0;

        snprintf(msg, sizeof(msg), "%s;%lld;%s;%s;%s;%d;%d",
                 DEVICE_ID, (long long)epoch_ms(),
                 PHASE_NAMES[s.phase], LIGHT_NAMES[car], LIGHT_NAMES[ped],
                 waiting, secs);
        mqtt_publish("lights", msg);

        digitalWrite(LED_PIN, waiting ? LOW : HIGH);

        xTaskDelayUntil(&last, pdMS_TO_TICKS(OUTPUT_PERIOD_MS));
    }
}

// TODO 3: TASK SPORADIC, tratează apăsările butonului pentru pietoni.
//   Apăsările vin din două surse, în aceeași coadă (coada butonului):
//   - butonul BOOT de pe placă, printr-o rutină de întrerupere (ISR, mai jos)
//   - butonul virtual din dashboard: comanda "BUTTON", pusă în coadă de
//     command_received()
//   Taskul:
//   - așteaptă o apăsare din coada butonului
//   - dacă de la ultima apăsare acceptată au trecut mai puțin de
//     MIN_INTERARRIVAL_MS, o respinge (mesaj pe Serial). Așa se filtrează și
//     contactele imperfecte ale butonului (bounce), care produc mai multe
//     întreruperi la o singură apăsare.
//   - altfel înregistrează cererea pietonului în starea semaforului (cu mutexul)
//     și afișează pe Serial că a acceptat-o
//   Momentul sosirii se ia când apăsarea intră în coadă (în ISR sau în
//   command_received()), nu când o preia taskul, și se pune în elementul cozii.
void button_task(void* arg) {
    ButtonPress      press;
    TickType_t       lastAccepted = 0;
    bool             hasLast      = false;
    const TickType_t minGap       = pdMS_TO_TICKS(MIN_INTERARRIVAL_MS);

    for (;;) {
        xQueueReceive(buttonQueue, &press, portMAX_DELAY);

        TickType_t gap = press.tick - lastAccepted;
        if (hasLast && gap < minGap) {
            Serial.printf("[buton] apăsare %c respinsă: %lu ms după ultima acceptată\n",
                          press.source, (unsigned long)(gap * portTICK_PERIOD_MS));
            continue;
        }
        hasLast      = true;
        lastAccepted = press.tick;

        bool already;
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        already          = state.pedRequest;
        state.pedRequest = true;
        xSemaphoreGive(stateMutex);

        Serial.printf("[buton] apăsare %c acceptată%s\n", press.source,
                      already ? " (cererea exista deja)" : ": AȘTEPTAȚI aprinsă");
    }
}

// TODO 3: RUTINA DE ÎNTRERUPERE a butonului BOOT, înregistrată în setup() cu
//   attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING).
//   Scurtă: pune o apăsare în coada butonului cu xQueueSendFromISR() și atât.
//   Fără Serial, fără mqtt_publish(), fără mutex. Se declară cu IRAM_ATTR:
//
//   void IRAM_ATTR button_isr(void) { ... }
void IRAM_ATTR button_isr(void) {
    ButtonPress press;
    press.tick   = xTaskGetTickCountFromISR();
    press.source = 'B';
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(buttonQueue, &press, &woken);
    portYIELD_FROM_ISR(woken);
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: "BUTTON" merge în coada butonului (o apăsare, la fel ca cea din
    //   ISR), fără să vă blocați (timeout 0). Celelalte comenzi se ignoră:
    //   în această temă placa nu are alte comenzi.
    while (*text == ' ') text++;
    const size_t n = 6;
    if (strncmp(text, "BUTTON", n) != 0) return;
    char after = text[n];
    if (after != '\0' && after != ' ' && after != '\r' && after != '\n') return;

    ButtonPress press;
    press.tick   = xTaskGetTickCount();
    press.source = 'D';
    if (xQueueSend(buttonQueue, &press, 0) != pdTRUE) {
        Serial.println("[mqtt] coada butonului plină, apăsare pierdută");
    }
}

void setup() {
    Serial.begin(115200);
    // Fără așteptare la scriere: cu placa la USB, dar fără Serial Monitor deschis,
    // fiecare Serial.printf() ar aștepta până la 2 s să fie citit și ar întârzia
    // taskul care scrie. Așa, textul pe care nu-l citește nimeni se pierde.
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 03: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    wifi_connect();
    time_sync();

    // TODO 0 (a): creați mutexul stării (xSemaphoreCreateMutex) și coada
    //   butonului (xQueueCreate). Înainte de mqtt_start(): de acolo încolo pot
    //   sosi comenzi, iar command_received() pune "BUTTON" în coadă.
    //   Notați și momentul intrării în faza inițială (CAR_GREEN).
    stateMutex  = xSemaphoreCreateMutex();
    buttonQueue = xQueueCreate(10, sizeof(ButtonPress));

    state.phase        = CAR_GREEN;
    state.phaseStartMs = millis();
    state.pedRequest   = false;

    mqtt_start();

    // TODO 0 (b): porniți cele trei taskuri (xTaskCreate), apoi înregistrați
    //   întreruperea butonului (attachInterrupt). Alegeți prioritățile între
    //   2 și 19 (peste loopTask, sub taskurile sistemului); pentru cele două
    //   taskuri periodice, după regula Rate Monotonic. Stiva, în octeți: cel puțin 4096.
    xTaskCreate(control_task, "control", TASK_STACK, NULL, CONTROL_PRIORITY, NULL);
    xTaskCreate(output_task,  "output",  TASK_STACK, NULL, OUTPUT_PRIORITY,  NULL);
    xTaskCreate(button_task,  "button",  TASK_STACK, NULL, BUTTON_PRIORITY,  NULL);

    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}