/*
 * LVGL configuration for this Arduino sketch.
 *
 * This file is intentionally minimal; LVGL will apply defaults for options not set here.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* Ensure LVGL picks this file up via "lv_conf.h" include */

/* Color depth: your ST7789 flush path is RGB565 (16-bit) */
#define LV_COLOR_DEPTH 16

/* Display resolution (used by some widgets/layout calculations) */
#define LV_HOR_RES_MAX 172
#define LV_VER_RES_MAX 320

/* Enable the built-in Montserrat fonts we use */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1

/* Disable logs by default (saves flash/RAM) */
#define LV_USE_LOG 0

/* Memory settings: keep defaults (LV_MEM_CUSTOM=0) unless you need otherwise */

/* Optional: reduce unused features for smaller builds (leave default if unsure) */

#endif /*LV_CONF_H*/
