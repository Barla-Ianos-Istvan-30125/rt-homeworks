package ssatr.service;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }
    @Override
    public void run() {
        try {
            while (true) {
                String message = inbox.take();

                // fiecare mesaj se salvează exact cum a sosit, chiar dacă e invalid
                storage.append(kind, message);

                try {
                    process(message);
                } catch (Exception e) {
                    // un mesaj greșit nu oprește thread-ul
                    System.out.println("[" + kind + "] mesaj invalid ignorat: '" + message + "' (" + e.getMessage() + ")");
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** Interpretează mesajul în funcție de tip și actualizează dashboard-ul. */
    private void process(String message) {
        switch (kind) {
            case "telemetry": {
                String[] f = fields(message, 3);       // deviceId;timestamp;temperatura
                dashboard.updateTelemetry(f[0], Double.parseDouble(f[2]));
                break;
            }
            case "alarm": {
                String[] f = fields(message, 4);       // deviceId;timestamp;temperatura;prag
                double temperature = Double.parseDouble(f[2]);
                double threshold = Double.parseDouble(f[3]);
                dashboard.registerAlarm(f[0], temperature);
                System.out.printf(Locale.US, "%n[ALARMA] %s: temperatura %.2f a depasit pragul %.2f%n",
                        f[0], temperature, threshold);
                break;
            }
            case "status": {
                String[] f = fields(message, 3);       // deviceId;timestamp;text
                dashboard.updateStatus(f[0], f[2]);
                System.out.printf("%n[STATUS] %s: %s%n", f[0], f[2]);
                break;
            }
            default:
                throw new IllegalStateException("tip de mesaj necunoscut: " + kind);
        }
    }

    /** Împarte mesajul în exact n câmpuri; ultimul câmp poate conține spații (textul de status). */
    private static String[] fields(String message, int n) {
        String[] f = message.split(";", n);
        if (f.length != n) {
            throw new IllegalArgumentException("se asteptau " + n + " campuri, am primit " + f.length);
        }
        return f;
    }
}
