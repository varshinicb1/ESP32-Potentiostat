#ifndef DISPLAY_LVGL_H
#define DISPLAY_LVGL_H

#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

// LovyanGFX driver for ILI9341 panel + XPT2046 touch on ESP32-S3
class LGFX_Potentiostat : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341  _panel_instance;
    lgfx::Bus_SPI        _bus_instance;
    lgfx::Touch_XPT2046  _touch_instance;

public:
    LGFX_Potentiostat();
};

class Display_LVGL {
private:
    static LGFX_Potentiostat tft;

    // LVGL HAL callbacks
    static void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p);
    static void my_touchpad_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data);

    // Screen objects (exactly one of each exists at a time)
    static lv_obj_t* mainScreen;
    static lv_obj_t* chartScreen;
    static lv_obj_t* chart;
    static lv_chart_series_t* series1;

public:
    static void init();
    static void handleLVGL();  // Call from UI task every ~5 ms

    // Navigation: always deletes old screen before creating new one
    static void buildMainScreen();
    static void showChartScreen(const char* techniqueName);

    // Data rendering
    static void addChartPoint(float x, float y);
    static void clearChart();

    // Safety / status screens
    static void showErrorScreen(const char* errorMessage);
    static void showStatusBar(const char* statusText, uint32_t color = 0x2E7D32);
};

#endif // DISPLAY_LVGL_H
