/**
 * Websocket driver for LittleVGL
 *
 * Contains a web server that serves a simple page to a client that draws the output
 * of LVGL into a canvas in a webserver via a websocket.  Mouse/Touch actions are returned
 * via the websocket for LVGL input.
 *
 * Networking must have been setup prior to starting this driver.
 *
 */
 #ifndef WEBSOCKET_DRIVER_H
 #define WEBSOCKET_DRIVER_H
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 /*********************
  *      INCLUDES
  *********************/
 #include "lvgl.h"
 #include <stdbool.h>
 #include <stdint.h>
 
 /*********************
  *      DEFINES
  *********************/
 #define DISP_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT)
 #define DEPTH_MASK 0x7F
 
 typedef enum { RGB565 = 16,
                       RGB332 = 8,
                       RGB121 = 4,
                       MONO4 = 0x84,
                       MONO2 = 0x82 } op_pixel_depth;
 
 /**********************
  * GLOBAL PROTOTYPES
  **********************/
 void websocket_driver_init(uint32_t bufSize, op_pixel_depth pixel_depth);
 bool websocket_driver_available();
 void websocket_driver_flush(lv_display_t *drv, const lv_area_t *area, lv_color_t *color_map);
 bool websocket_driver_read(lv_indev_t *drv, lv_indev_data_t *data);
 
 #ifdef __cplusplus
 } /* extern "C" */
 #endif
 
 #endif /* WEBSOCKET_DRIVER_H */