package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * TASK PERIODIC, cu perioada T = periodMs: fiecare job citește senzorul și
 * publică valoarea pe topic-ul de telemetrie.
 *
 * Dacă temperatura depășește pragul, taskul NU tratează el alarma, ci doar
 * semnalează un eveniment în coada alarmEvents. Alarma este tratată de
 * taskul sporadic.
 */
public class PeriodicTelemetryTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;

    // volatile: sunt citite aici, dar modificate de taskul aperiodic (alt thread)
    private volatile long periodMs = 2000;
    private volatile double threshold = 28.0;

    public PeriodicTelemetryTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                 BlockingQueue<Double> alarmEvents) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        // r0 = momentul primei eliberări; grila se calculează pe timp monoton
        long next = System.nanoTime();
        long k = 0;

        try {
            while (!Thread.currentThread().isInterrupted()) {
                // 1. așteptare ABSOLUTĂ până la eliberarea r_k de pe grilă
                long wait = next - System.nanoTime();
                if (wait > 0) {
                    TimeUnit.NANOSECONDS.sleep(wait);
                }

                // 2. jobul k: citire senzor + publicare
                long lateMs = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - next);
                double t = sensor.read();
                long timestamp = System.currentTimeMillis();
                String payload = deviceId + ";" + timestamp + ";" + String.format(Locale.US, "%.2f", t);

                mqtt.publish(Config.telemetryTopic(deviceId), payload);
                System.out.printf("[periodic] job %d  T=%d ms  intarziere=%d ms  -> %s%n",
                        k, periodMs, lateMs, payload);

                // 3. depășire de prag: doar semnalăm evenimentul, alarma e tratată de taskul sporadic
                if (t > threshold) {
                    alarmEvents.offer(t);
                }

                // 4. următoarea eliberare: r_{k+1} = r_k + T (periodMs citit din nou, poate fi schimbat)
                long periodNs = TimeUnit.MILLISECONDS.toNanos(periodMs);
                next += periodNs;

                // dacă jobul a depășit perioada, sărim peste eliberările pierdute, rămânând pe grilă
                long now = System.nanoTime();
                if (now > next) {
                    long missed = (now - next) / periodNs + 1;
                    next += missed * periodNs;
                }
                k++;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt(); // oprire curată a taskului
        }
    }

    public void setPeriodMs(long periodMs) {
        this.periodMs = periodMs;
    }

    public void setThreshold(double threshold) {
        this.threshold = threshold;
    }

    public long getPeriodMs() {
        return periodMs;
    }

    public double getThreshold() {
        return threshold;
    }
}
