#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <stdlib.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/activity.h>
#include <zmk/keymap.h>

#define STRIP_NODE DT_NODELABEL(pad15_leds)
#define NUM_PIXELS 16
#define STATUS_LED_IDX 15
#define MAX_EFFECTS 4                      
#define BRIGHTNESS_PERCENT 30              

static const struct device *led_strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[NUM_PIXELS];

// ==========================================
// 状态全局变量
// ==========================================
static uint8_t battery_level = 100;
static int status_display_frames = 66;     
static uint8_t current_effect = 3;         // 默认开启对角线瀑布幻彩
static bool is_awake = true;
static uint8_t tracked_layer = 0;          // 锁定开机必然是第 0 层 (解决开机闪绿灯Bug)

// ==========================================
// 工业标准 HSV 引擎 (彻底解决颜色缺失与亮度闪烁)
// h: 色相(0-255，覆盖完整的红橙黄绿青蓝紫256种颜色)
// s: 饱和度(固定255，最艳丽)
// v: 亮度(固定255，绝对恒定亮度，消灭任何呼吸/熄灭感)
// ==========================================
static struct led_rgb hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
    struct led_rgb rgb;
    uint8_t region, remainder, p, q, t;

    if (s == 0) {
        rgb.r = v; rgb.g = v; rgb.b = v;
        return rgb;
    }

    region = h / 43; // 256 / 6 个区域
    remainder = (h - (region * 43)) * 6; 

    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
        case 0: rgb.r = v; rgb.g = t; rgb.b = p; break; // 红 -> 黄
        case 1: rgb.r = q; rgb.g = v; rgb.b = p; break; // 黄 -> 绿
        case 2: rgb.r = p; rgb.g = v; rgb.b = t; break; // 绿 -> 青
        case 3: rgb.r = p; rgb.g = q; rgb.b = v; break; // 青 -> 蓝
        case 4: rgb.r = t; rgb.g = p; rgb.b = v; break; // 蓝 -> 紫
        default: rgb.r = v; rgb.g = p; rgb.b = q; break; // 紫 -> 红
    }
    return rgb;
}

// ==========================================
// 核心动画线程
// ==========================================
void custom_led_thread_main(void) {
    uint32_t tick = 0;
    if (!device_is_ready(led_strip)) {
        printk("Custom LED: WS2812 strip not ready!\n");
        return;
    } 

    while (1) {
        if (!is_awake) {
            for (int i = 0; i < NUM_PIXELS; i++) pixels[i] = (struct led_rgb){0, 0, 0};
            led_strip_update_rgb(led_strip, pixels, NUM_PIXELS);
            k_sleep(K_MSEC(500)); 
            continue;
        }

        // --- 1. 渲染前 15 颗矩阵灯 (256色全光谱平滑过渡) ---
        for (int i = 0; i < STATUS_LED_IDX; i++) {
            uint8_t col = i % 3; 
            uint8_t row = i / 3; 

            if (current_effect == 0) {
                // 全局同步变色
                pixels[i] = hsv_to_rgb((uint8_t)(tick * 2), 255, 255); 
            } 
            else if (current_effect == 1) {
                // 横向流动 (左 -> 右) 【注意：这里改成了减号 -，修正方向】
                pixels[i] = hsv_to_rgb((uint8_t)(tick * 2 - col * 30), 255, 255);
            }
            else if (current_effect == 2) {
                // 纵向瀑布 (上 -> 下) 【注意：这里改成了减号 -，修正方向】
                pixels[i] = hsv_to_rgb((uint8_t)(tick * 2 - row * 25), 255, 255);
            }
            else if (current_effect == 3) {
                // 斜向瀑布 (左上 -> 右下) 【注意：双重减号，完全按照从上到下的视觉习惯】
                pixels[i] = hsv_to_rgb((uint8_t)(tick * 2 - col * 20 - row * 20), 255, 255);
            }
        }

        // --- 2. 渲染第 16 颗状态灯 ---
        if (battery_level > 0 && battery_level < 10) {
            if (tick % 30 < 15) pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x00, 0x00};
            else pixels[STATUS_LED_IDX] = (struct led_rgb){0, 0, 0};
        } 
        else if (status_display_frames > 0) {
            switch (tracked_layer) {
                case 0: pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x50, 0x90}; break; // 粉嫩樱花色
                case 1: pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0x80, 0x00}; break; // 纯正橙色
                case 2: pixels[STATUS_LED_IDX] = (struct led_rgb){0x00, 0xFF, 0x00}; break; // 纯正绿色
                case 3: pixels[STATUS_LED_IDX] = (struct led_rgb){0x00, 0x80, 0xFF}; break; // 纯正天蓝
                default: pixels[STATUS_LED_IDX] = (struct led_rgb){0xFF, 0xFF, 0xFF}; break; 
            }
            status_display_frames--; 
        } 
        else {
            pixels[STATUS_LED_IDX] = (struct led_rgb){0, 0, 0}; 
        }
        
        // --- 3. 全局亮度限制 ---
        for (int i = 0; i < NUM_PIXELS; i++) {
            pixels[i].r = (pixels[i].r * BRIGHTNESS_PERCENT) / 100;
            pixels[i].g = (pixels[i].g * BRIGHTNESS_PERCENT) / 100;
            pixels[i].b = (pixels[i].b * BRIGHTNESS_PERCENT) / 100;
        }
                
        led_strip_update_rgb(led_strip, pixels, NUM_PIXELS);
        tick++;
        k_sleep(K_MSEC(30)); // 动画帧率 (降低数值可加快流动速度)
    }
}
K_THREAD_DEFINE(custom_led_tid, 1024, custom_led_thread_main, NULL, NULL, NULL, 7, 0, 0);

// ==========================================
// 绝对安全的 ZMK 事件接收器
// ==========================================

static int activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev) {
        if (ev->state == ZMK_ACTIVITY_ACTIVE) {
            is_awake = true;
            status_display_frames = 66; 
        } else {
            is_awake = false; 
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(activity_status, activity_listener);
ZMK_SUBSCRIPTION(activity_status, zmk_activity_state_changed);

static int keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev && ev->state) {
        if (ev->keycode == 0x6A) {
            current_effect++;
            if (current_effect >= MAX_EFFECTS) current_effect = 0;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(keycode_status, keycode_listener);
ZMK_SUBSCRIPTION(keycode_status, zmk_keycode_state_changed);

static int layer_status_listener(const zmk_event_t *eh) {
    tracked_layer = zmk_keymap_highest_layer_active();
    status_display_frames = 66; 
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(layer_status, layer_status_listener);
ZMK_SUBSCRIPTION(layer_status, zmk_layer_state_changed);

static int battery_status_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        battery_level = ev->state_of_charge;
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(battery_status, battery_status_listener);
ZMK_SUBSCRIPTION(battery_status, zmk_battery_state_changed);
