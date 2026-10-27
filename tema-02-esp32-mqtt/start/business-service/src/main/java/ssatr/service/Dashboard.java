package ssatr.service;

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * Ultima stare cunoscută a fiecărui device.
 * Actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci toate metodele sunt synchronized.
 */
public class Dashboard {

    /** Starea unui device. */
    private static class DeviceState {
        double temperature = Double.NaN;  // ultima temperatură primită
        long lastMessageMs;               // momentul (local) la care a sosit ultimul mesaj
        int alarms;                       // numărul de alarme primite
        String lastStatus = "-";          // ultima confirmare de comandă
    }

    // TreeMap: device-urile apar sortate după ID în tabel
    private final Map<String, DeviceState> devices = new TreeMap<>();

    private DeviceState state(String deviceId) {
        DeviceState s = devices.computeIfAbsent(deviceId, id -> new DeviceState());
        s.lastMessageMs = System.currentTimeMillis();
        return s;
    }

    public synchronized void updateTelemetry(String deviceId, double temperature) {
        state(deviceId).temperature = temperature;
    }

    public synchronized void registerAlarm(String deviceId, double temperature) {
        DeviceState s = state(deviceId);
        s.temperature = temperature;
        s.alarms++;
    }

    public synchronized void updateStatus(String deviceId, String text) {
        state(deviceId).lastStatus = text;
    }

    /** Afișează un tabel cu starea tuturor device-urilor. */
    public synchronized void print() {
        if (devices.isEmpty()) {
            System.out.println("(niciun device nu a trimis date încă)");
            return;
        }
        long now = System.currentTimeMillis();
        System.out.printf("%-12s %8s %16s %7s   %s%n", "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME", "ULTIMUL STATUS");
        for (Map.Entry<String, DeviceState> e : devices.entrySet()) {
            DeviceState s = e.getValue();
            String temp = Double.isNaN(s.temperature) ? "-" : String.format(Locale.US, "%.2f", s.temperature);
            String age = "acum " + (now - s.lastMessageMs) / 1000 + " s";
            System.out.printf("%-12s %8s %16s %7d   %s%n", e.getKey(), temp, age, s.alarms, s.lastStatus);
        }
    }
}