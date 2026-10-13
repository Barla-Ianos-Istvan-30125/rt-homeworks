package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

public class PeriodicTelemetryTask implements Runnable {
    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;

    // volatile ca sa se vada imediat cand le schimba alt thread
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
        // r0 - ceas monoton, nu e afectat de ora sistemului
        long next = System.nanoTime();
        long k = 0;

        try {
            while (!Thread.currentThread().isInterrupted()) {
                // asteptare absoluta pana la eliberarea r_k
                long wait = next - System.nanoTime();
                if (wait > 0) {
                    TimeUnit.NANOSECONDS.sleep(wait);
                }

                long lateMs = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - next);
                double t = sensor.read();
                long timestamp = System.currentTimeMillis();
                String payload = deviceId + ";" + timestamp + ";" + String.format(Locale.US, "%.2f", t);

                // trimit valoarea pe topicul telemetry
                mqtt.publish(Config.telemetryTopic(deviceId), payload);
                System.out.printf("[periodic] job %d  T=%d ms  intarziere=%d ms  -> %s%n",
                        k, periodMs, lateMs, payload);

                // depasire de prag -> semnal in coada de alarme
                if (t > threshold) {
                    alarmEvents.offer(t);
                }

                // r_{k+1} = r_k + T, perioada noua se aplica de la urmatorul job
                long periodNs = TimeUnit.MILLISECONDS.toNanos(periodMs);
                next += periodNs;

                // recuperare dupa depasire: sar peste eliberarile pierdute
                long now = System.nanoTime();
                if (now > next) {
                    long missed = (now - next) / periodNs + 1;
                    next += missed * periodNs;
                }
                k++;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
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