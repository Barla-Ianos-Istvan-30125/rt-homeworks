package ssatr.device;

import java.util.Random;


public class TemperatureSensor {

    private static final double INITIAL = 22.0;

    private final Random random = new Random();
    private double value = INITIAL;

    public synchronized double read() {
        value += (random.nextDouble() - 0.5);
        if (random.nextInt(10) == 0) {
            value += 3 + random.nextDouble() * 5;
        }
        if (value > INITIAL + 2) {
            value -= 1;
        }
        return value;
    }

    public synchronized void reset() {
        value = INITIAL;
    }
}
