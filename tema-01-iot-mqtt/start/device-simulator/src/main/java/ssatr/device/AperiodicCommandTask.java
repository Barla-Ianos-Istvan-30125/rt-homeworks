package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

public class AperiodicCommandTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<String> commands;
    private final PeriodicTelemetryTask periodic;

    public AperiodicCommandTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                BlockingQueue<String> commands, PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.commands = commands;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        try {
            while (true) {
                String command = commands.take();
                long start = System.currentTimeMillis();

                String result;
                try {
                    result = "OK " + execute(command);
                } catch (Exception e) {

                    result = "ERROR " + e.getMessage();
                }

                publishStatus(result);
                System.out.printf("[aperiodic] '%s' -> %s (executie %d ms, in coada: %d)%n",
                        command, result, System.currentTimeMillis() - start, commands.size());
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }


    private String execute(String command) {
        String[] parts = command.trim().split("\\s+");
        String name = parts[0].toUpperCase();

        switch (name) {
            case "SET_INTERVAL": {
                long ms = parseLong(parts, name);
                if (ms < 100) {
                    throw new IllegalArgumentException("SET_INTERVAL: perioada trebuie sa fie >= 100 ms");
                }
                periodic.setPeriodMs(ms);
                return "SET_INTERVAL " + ms;
            }
            case "SET_THRESHOLD": {
                double value = parseDouble(parts, name);
                periodic.setThreshold(value);
                return "SET_THRESHOLD " + String.format(Locale.US, "%.2f", value);
            }
            case "RESET":
                sensor.reset();
                return "RESET";
            case "STATUS":
                return String.format(Locale.US, "STATUS interval=%d threshold=%.2f",
                        periodic.getPeriodMs(), periodic.getThreshold());
            case "":
                throw new IllegalArgumentException("comanda goala");
            default:
                throw new IllegalArgumentException("comanda necunoscuta: " + parts[0]);
        }
    }

    private long parseLong(String[] parts, String name) {
        if (parts.length < 2) {
            throw new IllegalArgumentException(name + ": lipseste parametrul");
        }
        try {
            return Long.parseLong(parts[1]);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException(name + ": parametru invalid '" + parts[1] + "'");
        }
    }

    private double parseDouble(String[] parts, String name) {
        if (parts.length < 2) {
            throw new IllegalArgumentException(name + ": lipseste parametrul");
        }
        try {
            return Double.parseDouble(parts[1]);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException(name + ": parametru invalid '" + parts[1] + "'");
        }
    }


    private void publishStatus(String text) {
        mqtt.publish(Config.statusTopic(deviceId), deviceId + ";" + System.currentTimeMillis() + ";" + text);
    }
}
