#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

void power_init(void) {
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    // ---- Power rails for the LCD panel ----
    // The ST7796 panel + its logic are fed from AXP2101 rails. Without these
    // enabled the panel is unpowered and the screen stays black even though
    // SPI writes and the backlight GPIO "succeed". Voltages and the enabled
    // set mirror the board's factory firmware exactly.
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
}
