#include "Display_LVGL.h"

// Initialize static members
LGFX_Potentiostat Display_LVGL::tft;
lv_obj_t* Display_LVGL::mainScreen  = nullptr;
lv_obj_t* Display_LVGL::chartScreen = nullptr;
lv_obj_t* Display_LVGL::chart       = nullptr;
lv_chart_series_t* Display_LVGL::series1 = nullptr;

// LVGL draw buffer — double-buffered for smooth rendering (1/5 of screen)
#define LV_BUF_SIZE (320 * 240 / 5)
static lv_color_t buf1[LV_BUF_SIZE];
static lv_color_t buf2[LV_BUF_SIZE];

// ===================================================================
// LGFX_Potentiostat Constructor — hardware pin configuration
// ===================================================================
LGFX_Potentiostat::LGFX_Potentiostat() {
    // SPI Bus (shared with display panel)
    auto bus_cfg = _bus_instance.config();
    bus_cfg.spi_mode   = 0;
    bus_cfg.freq_write = 40000000; // 40 MHz write
    bus_cfg.freq_read  = 16000000; // 16 MHz read
    bus_cfg.pin_sclk   = 36;
    bus_cfg.pin_mosi   = 35;
    bus_cfg.pin_miso   = 37;
    bus_cfg.pin_dc     = 21;
    _bus_instance.config(bus_cfg);
    _panel_instance.setBus(&_bus_instance);

    // TFT Panel — ILI9341 2.8" 320×240
    auto panel_cfg = _panel_instance.config();
    panel_cfg.pin_cs          = 47;
    panel_cfg.pin_rst         = 38;
    // pin_bl is not a Panel_Device field in LovyanGFX v1.x;
    // backlight is controlled separately via direct GPIO.
    panel_cfg.panel_width     = 240;
    panel_cfg.panel_height    = 320;
    panel_cfg.offset_x        = 0;
    panel_cfg.offset_y        = 0;
    _panel_instance.config(panel_cfg);

    // Touchscreen — XPT2046 resistive (on SPI2_HOST, separate from display)
    auto touch_cfg = _touch_instance.config();
    touch_cfg.pin_cs   = 48;
    touch_cfg.pin_int  = 40;
    touch_cfg.spi_host = SPI2_HOST;
    touch_cfg.x_min    = 300;
    touch_cfg.x_max    = 3900;
    touch_cfg.y_min    = 200;
    touch_cfg.y_max    = 3800;
    _touch_instance.config(touch_cfg);
    _panel_instance.setTouch(&_touch_instance);

    setPanel(&_panel_instance);
}

// ===================================================================
// LVGL display flush callback
// ===================================================================
void Display_LVGL::my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.writePixels((uint16_t*)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

// ===================================================================
// LVGL touch input callback
// ===================================================================
void Display_LVGL::my_touchpad_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data) {
    uint16_t touchX, touchY;
    bool touched = tft.getTouch(&touchX, &touchY);
    if (!touched) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;
    }
}

// ===================================================================
// init()
// ===================================================================
void Display_LVGL::init() {
    tft.init();
    tft.setRotation(1); // Landscape: 320x240

    // Enable backlight directly on GPIO 45 (LovyanGFX v1.x configures BL separately)
    pinMode(45, OUTPUT);
    digitalWrite(45, HIGH);

    lv_init();

    // Double-buffered draw buffer for tear-free rendering
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LV_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = 320;
    disp_drv.ver_res  = 240;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    buildMainScreen();
}

// ===================================================================
// handleLVGL() — must be called every ~5 ms from TaskUI
// ===================================================================
void Display_LVGL::handleLVGL() {
    lv_timer_handler();
}

// ===================================================================
// UI Event: main screen button handler
// ===================================================================
static void btn_event_cb(lv_event_t* e) {
    lv_obj_t*  btn   = lv_event_get_target(e);
    lv_obj_t*  label = lv_obj_get_child(btn, 0);
    const char* txt  = lv_label_get_text(label);

    if      (strcmp(txt, "Cyclic Voltammetry") == 0) Display_LVGL::showChartScreen("CV");
    else if (strcmp(txt, "Chronoamperometry")  == 0) Display_LVGL::showChartScreen("CA");
    else if (strcmp(txt, "Square Wave Volt")   == 0) Display_LVGL::showChartScreen("SWV");
    else if (strcmp(txt, "Impedance (EIS)")    == 0) Display_LVGL::showChartScreen("EIS");
}

// ===================================================================
// buildMainScreen()
// Deletes any existing mainScreen object before creating a new one
// to prevent LVGL heap leaks on repeated "Back" navigation.
// ===================================================================
void Display_LVGL::buildMainScreen() {
    // Delete old screen objects first
    if (chartScreen) {
        lv_obj_del(chartScreen);
        chartScreen = nullptr;
        chart   = nullptr;
        series1 = nullptr;
    }
    if (mainScreen) {
        lv_obj_del(mainScreen);
        mainScreen = nullptr;
    }

    mainScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(mainScreen, lv_color_hex(0x121212), 0);

    // Title
    lv_obj_t* title = lv_label_create(mainScreen);
    lv_label_set_text(title, "VidyuthLabs Potentiostat");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x64B5F6), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Subtitle
    lv_obj_t* sub = lv_label_create(mainScreen);
    lv_label_set_text(sub, "Select a technique to begin");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 32);

    // Button style
    static lv_style_t btn_style;
    lv_style_init(&btn_style);
    lv_style_set_bg_color(&btn_style, lv_color_hex(0x1565C0));
    lv_style_set_bg_opa(&btn_style, LV_OPA_COVER);
    lv_style_set_text_color(&btn_style, lv_color_white());
    lv_style_set_radius(&btn_style, 10);
    lv_style_set_shadow_width(&btn_style, 6);
    lv_style_set_shadow_color(&btn_style, lv_color_hex(0x0D47A1));

    // Helper lambda to create a technique button
    auto makeBtn = [&](const char* label, lv_align_t align, int32_t xOff, int32_t yOff) {
        lv_obj_t* btn = lv_btn_create(mainScreen);
        lv_obj_add_style(btn, &btn_style, 0);
        lv_obj_set_size(btn, 130, 50);
        lv_obj_align(btn, align, xOff, yOff);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);
    };

    makeBtn("Cyclic Voltammetry", LV_ALIGN_CENTER, -75, -30);
    makeBtn("Chronoamperometry",  LV_ALIGN_CENTER,  75, -30);
    makeBtn("Square Wave Volt",   LV_ALIGN_CENTER, -75,  35);
    makeBtn("Impedance (EIS)",    LV_ALIGN_CENTER,  75,  35);

    lv_scr_load(mainScreen);
}

// ===================================================================
// showChartScreen()
// Deletes any existing chartScreen before creating a new one.
// ===================================================================
void Display_LVGL::showChartScreen(const char* techniqueName) {
    // Destroy old chart screen to free LVGL heap
    if (chartScreen) {
        lv_obj_del(chartScreen);
        chartScreen = nullptr;
        chart   = nullptr;
        series1 = nullptr;
    }

    chartScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(chartScreen, lv_color_hex(0x121212), 0);

    // Technique Title
    lv_obj_t* title = lv_label_create(chartScreen);
    lv_label_set_text_fmt(title, "%s — Live Plot", techniqueName);
    lv_obj_set_style_text_color(title, lv_color_hex(0x64B5F6), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Back Button
    lv_obj_t* back_btn = lv_btn_create(chartScreen);
    lv_obj_set_size(back_btn, 70, 30);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x424242), 0);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, [](lv_event_t* e) {
        Display_LVGL::buildMainScreen();
    }, LV_EVENT_CLICKED, NULL);

    // Chart widget
    chart = lv_chart_create(chartScreen);
    lv_obj_set_size(chart, 290, 170);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -1000, 1000);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_X,    0,   100);
    lv_chart_set_point_count(chart, 200);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x1A1A1A), 0);
    lv_chart_set_div_line_count(chart, 5, 5);

    // Single series (µA values)
    series1 = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_CYAN), LV_CHART_AXIS_PRIMARY_Y);

    lv_scr_load(chartScreen);
}

// ===================================================================
// addChartPoint() — appends a new Y-value to series1
// ===================================================================
void Display_LVGL::addChartPoint(float /*x*/, float y) {
    if (chart && series1) {
        // y is expected in µA (caller multiplies raw amps by 1e6)
        // LVGL chart uses integer values; clamp to int32 range
        int32_t yInt = (int32_t)y;
        if (yInt < -32768) yInt = -32768;
        if (yInt >  32767) yInt =  32767;
        lv_chart_set_next_value(chart, series1, yInt);
        lv_chart_refresh(chart);
    }
}

// ===================================================================
// clearChart()
// ===================================================================
void Display_LVGL::clearChart() {
    if (chart && series1) {
        // lv_chart_clear_series() was removed in LVGL 8.x.
        // Reset all buffered points to the 'no data' sentinel instead.
        lv_chart_set_all_value(chart, series1, LV_CHART_POINT_NONE);
        lv_chart_refresh(chart);
    }
}

// ===================================================================
// showErrorScreen() — full-screen critical error display (no delete needed;
// this is called only when halting, so leaking the object is acceptable)
// ===================================================================
void Display_LVGL::showErrorScreen(const char* errorMessage) {
    lv_obj_t* errorScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(errorScreen, lv_color_hex(0x7F0000), 0);

    // Warning icon
    lv_obj_t* icon = lv_label_create(errorScreen);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 25);

    // Title
    lv_obj_t* titleLabel = lv_label_create(errorScreen);
    lv_label_set_text(titleLabel, "SYSTEM ERROR");
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(titleLabel, lv_color_white(), 0);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 65);

    // Error detail
    lv_obj_t* msgLabel = lv_label_create(errorScreen);
    lv_label_set_text(msgLabel, errorMessage);
    lv_label_set_long_mode(msgLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msgLabel, 290);
    lv_obj_set_style_text_color(msgLabel, lv_color_hex(0xFFCCCC), 0);
    lv_obj_set_style_text_align(msgLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msgLabel, LV_ALIGN_CENTER, 0, 5);

    // Instruction
    lv_obj_t* instr = lv_label_create(errorScreen);
    lv_label_set_text(instr, "Power cycle the device to retry");
    lv_obj_set_style_text_color(instr, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(instr, LV_ALIGN_BOTTOM_MID, 0, -15);

    lv_scr_load(errorScreen);
}

// ===================================================================
// showStatusBar() — overlays a small status bar on the current screen
// ===================================================================
void Display_LVGL::showStatusBar(const char* statusText, uint32_t color) {
    lv_obj_t* activeScreen = lv_scr_act();
    if (!activeScreen) return;

    lv_obj_t* bar = lv_obj_create(activeScreen);
    lv_obj_set_size(bar, 320, 22);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_80, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 2, 0);
    lv_obj_set_style_border_width(bar, 0, 0);

    lv_obj_t* label = lv_label_create(bar);
    lv_label_set_text(label, statusText);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}
