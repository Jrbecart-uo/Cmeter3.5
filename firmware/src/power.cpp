#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

// Poll intervals
#define BATTERY_POLL_MS   2000
#define CHARGING_POLL_MS  500

static int      cached_pct      = -1;
static bool     cached_charging = false;
static bool     pwr_pressed_flag = false;
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;
#define PWR_POLL_MS 50

void power_init(void) {
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    // ---- Power rails for the LCD panel ----
    // The AXS15231B panel + its logic are fed from AXP2101 rails. Without
    // these enabled the panel chip is unpowered and the screen stays black
    // even though QSPI writes and the backlight GPIO "succeed". Voltages and
    // the enabled set mirror the board's factory firmware
    // (device/ESP-IDF/01_factory .../bsp_axp2101.cpp) exactly.
    pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
    pmu.setSysPowerDownVoltage(2600);

    pmu.setDC1Voltage(3300);
    pmu.setDC2Voltage(1000);
    pmu.setDC3Voltage(3300);
    pmu.setDC4Voltage(1000);
    pmu.setDC5Voltage(3300);
    pmu.setALDO1Voltage(3300);
    pmu.setALDO2Voltage(3300);
    pmu.setALDO3Voltage(3300);
    pmu.setALDO4Voltage(3300);
    pmu.setBLDO1Voltage(1500);
    pmu.setBLDO2Voltage(2800);
    pmu.setCPUSLDOVoltage(1000);
    pmu.setDLDO1Voltage(3300);
    pmu.setDLDO2Voltage(3300);

    pmu.enableDC2();
    pmu.enableDC3();
    pmu.enableDC4();
    pmu.enableDC5();
    pmu.enableALDO1();
    pmu.enableALDO2();
    pmu.enableALDO3();
    pmu.enableALDO4();
    pmu.enableBLDO1();
    pmu.enableBLDO2();
    pmu.enableCPUSLDO();
    pmu.enableDLDO1();
    pmu.enableDLDO2();

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();

    // Enable PWR button short-press IRQ (mid button for cycling screens)
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

    cached_charging = pmu.isCharging();
    cached_pct = pmu.getBatteryPercent();
}

void power_tick(void) {
    uint32_t now = millis();

    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging = pmu.isCharging();
    }

    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = pmu.getBatteryPercent();
    }

    // Poll PWR button (AXP2101 short-press IRQ)
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        pmu.getIrqStatus();
        if (pmu.isPekeyShortPressIrq()) {
            pwr_pressed_flag = true;
        }
        pmu.clearIrqStatus();
    }
}

int power_battery_pct(void) {
    return cached_pct;
}

bool power_is_charging(void) {
    return cached_charging;
}

bool power_pwr_pressed(void) {
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
    return false;
}
