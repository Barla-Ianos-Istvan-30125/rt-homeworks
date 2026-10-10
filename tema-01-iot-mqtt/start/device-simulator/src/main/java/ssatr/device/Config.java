package ssatr.device;

public class Config {

    public static final String BROKER = "tcp://control.aut.utcluj.ro:11188";

    public static final String STUDENT = "barla-ianos-istvan";

    private static final String DEVICES = "ssatr/" + STUDENT + "/devices/";


    public static String telemetryTopic(String deviceId) {
        return DEVICES + deviceId + "/telemetry";
    }


    public static String alarmTopic(String deviceId) {
        return DEVICES + deviceId + "/alarm";
    }


    public static String commandTopic(String deviceId) {
        return DEVICES + deviceId + "/commands";
    }


    public static String statusTopic(String deviceId) {
        return DEVICES + deviceId + "/status";
    }
}
