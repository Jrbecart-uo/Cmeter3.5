#pragma once

// Enables the AXP2101 power rails that feed the ST7796 LCD panel.
// Must run before the TCA9554 reset pulse / gfx->begin(). The device is
// USB-powered only (no LiPo, no buttons) so there is no battery readout.
void power_init(void);
