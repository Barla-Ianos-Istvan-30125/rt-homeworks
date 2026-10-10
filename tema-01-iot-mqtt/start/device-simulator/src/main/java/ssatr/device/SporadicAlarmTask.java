package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 * Impune intervalul minim între sosiri MIN_INTERARRIVAL_MS:
 * evenimentele care sosesc mai des sunt respinse.
 */
public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;
    static final long DEADLINE_MS = 300;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;
    private final PeriodicTelemetryTask periodic;   // de aici citim pragul curent

    private long lastRelease = 0;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<Double> alarmEvents,
                             PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        try {
            while (true) {
                double t = alarmEvents.take();              // așteaptă (blocat) un eveniment
                long release = System.currentTimeMillis();  // eliberarea jobului

                long sinceLast = release - lastRelease;
                if (sinceLast < MIN_INTERARRIVAL_MS) {
                    System.out.printf(Locale.US, "[sporadic] RESPINS t=%.2f: au trecut doar %d ms (minim %d ms)%n",
                            t, sinceLast, MIN_INTERARRIVAL_MS);
                    continue;
                }
                lastRelease = release;

                // jobul: publicarea alarmei
                String payload = deviceId + ";" + release + ";"
                        + String.format(Locale.US, "%.2f", t) + ";"
                        + String.format(Locale.US, "%.2f", periodic.getThreshold());
                mqtt.publish(Config.alarmTopic(deviceId), payload);

                long responseTime = System.currentTimeMillis() - release;
                System.out.printf("[sporadic] ALARMA R=%d ms -> %s (D=%d ms) -> %s%n",
                        responseTime, responseTime <= DEADLINE_MS ? "termen respectat" : "TERMEN RATAT",
                        DEADLINE_MS, payload);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}