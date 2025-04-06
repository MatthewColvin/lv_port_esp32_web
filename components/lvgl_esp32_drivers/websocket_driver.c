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

/*********************
 *      INCLUDES
 *********************/
 #include "websocket_driver.h"
 #include "esp_system.h"
 #include "esp_log.h"
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 #include "freertos/queue.h"
 #include "freertos/semphr.h"
 #include "lwip/api.h"
 #include "lwip/tcp.h"
 #include "lvgl.h"
 #include "string.h"
 #include "../websocket/include/websocket.h"
 #include "../websocket/include/websocket_server.h"
 
 
 /*********************
  *      DEFINES
  *********************/
 #define PIXEL_BUF_HEADER_LEN  13
 #define WS_BUF_HEADER_MAX_LEN 10
 #define STATIC_BUF_EXTRA_LEN  (PIXEL_BUF_HEADER_LEN + WS_BUF_HEADER_MAX_LEN)
 
 
 /**********************
  *      TYPEDEFS
  **********************/
 typedef struct
 {
	 SemaphoreHandle_t mutex;
	 uint8_t flag;
	 uint16_t x;
	 uint16_t y;
 } pointer_t;
 
 
 /**********************
  *  STATIC VARIABLES
  **********************/
 static const char* TAG = "websocket_driver";
  
 // Connection state
 static bool websocket_connected = false;
 
 // Inner-task queue
 static QueueHandle_t client_queue;
 const static int client_queue_size = 5;
 
 // Pre-allocated websocket buffer containing combined websocket header and pixel data
 static uint8_t *msg_buf;//[DISP_BUF_SIZE * (LV_COLOR_DEPTH/8) + STATIC_BUF_EXTRA_LEN];
 
 // Pixel depth in bits
 static op_pixel_depth output_pixel_depth;
 
 static bool enabled = true;
 
 // Current pointer value
 static pointer_t pointer;
 
 
 /**********************
  *  STATIC PROTOTYPES
  **********************/
 static void websocket_callback(uint8_t num, WEBSOCKET_TYPE_t type, char* msg, uint64_t len);
 static void http_serve(struct netconn *conn);
 static void server_task(void* pvParameters);
 static void server_handle_task(void* pvParameters);
 static int set_msg_buf(uint32_t msg_len);
 static int ws_send_nocopy_bin_all(uint32_t len);
 static int num_connected_clients();
 
  
 /**********************
  *   GLOBAL FUNCTIONS
  **********************/
 void websocket_driver_init(uint32_t bufSize, op_pixel_depth pixel_depth)
 {
   output_pixel_depth = pixel_depth;
 
	 ESP_LOGI(TAG, "Initialization.");
   if(LV_COLOR_DEPTH != 16) {
	 ESP_LOGI(TAG, "Error - Unsupported color depth.");
	 enabled = false;
	 return;
   }
 
   msg_buf = (uint8_t *)malloc(bufSize + STATIC_BUF_EXTRA_LEN);
   if(msg_buf == NULL) {
	 enabled = false;
	 ESP_LOGI(TAG, "Error - Memory could not be allocated");
   }
	 
	 ws_server_start();
	 xTaskCreate(&server_task, "server_task", 3000, NULL, 9, NULL);
	 xTaskCreate(&server_handle_task, "server_handle_task", 4000, NULL, 6, NULL);
	 
	 pointer.mutex = xSemaphoreCreateMutex();
	 pointer.flag = 0;
	 pointer.x = 0;
	 pointer.y = 0;
 }
 
 
 bool websocket_driver_available()
 {
	 return websocket_connected;
 }
 
 
 void websocket_driver_flush(lv_disp_t * drv, const lv_area_t * area, lv_color_t * color_map)
 {
	 int i;
	 int hdr_len;
	 int w, h;
	 uint8_t* buf;
	 uint32_t size;
	 
	 if (websocket_connected && enabled) {
	 size = lv_area_get_width(area) * lv_area_get_height(area);
		 w = lv_disp_get_hor_res(NULL);
		 h = lv_disp_get_ver_res(NULL);
 
	 uint32_t output_size = (size * (output_pixel_depth&DEPTH_MASK))/8;
	 if((output_pixel_depth&DEPTH_MASK)<8)
	   if((output_size%(8/(output_pixel_depth&DEPTH_MASK))) != 0) 
		 output_size++;
 
		 // Setup the direct websocket message header.  buf is set to the first location
		 // of the websocket payload.  The websocket message header is filled in.
		 hdr_len = set_msg_buf(output_size + PIXEL_BUF_HEADER_LEN);
		 buf = &msg_buf[hdr_len];
 
		 // Add a binary message containing the coordinates and 32-bit pixel
		 // data.  This must match the javascript unpacking routine in index.html.
		 // This way we don't have to worry about endianness.
		 //
		 // Load the region coordinates
		 *buf++ = (uint8_t)output_pixel_depth;
		 *buf++ = (w >> 8) & 0xFF;
		 *buf++ =  w       & 0xFF;
		 *buf++ = (h >> 8) & 0xFF;
		 *buf++ =  h       & 0xFF;
		 *buf++ = (area->x1 >> 8) & 0xFF;
		 *buf++ =  area->x1       & 0xFF;
		 *buf++ = (area->y1 >> 8) & 0xFF;
		 *buf++ =  area->y1       & 0xFF;
		 *buf++ = (area->x2 >> 8) & 0xFF;
		 *buf++ =  area->x2       & 0xFF;
		 *buf++ = (area->y2 >> 8) & 0xFF;
		 *buf++ =  area->y2       & 0xFF;
			 
		 if (output_pixel_depth == RGB565) {
			 // Load the 16-bit pixel data: RGB565
			 uint16_t *p16 = (uint16_t *) color_map;
			 for (i=0; i<size; i++) {
				 *buf++ = p16[i] >> 8;
				 *buf++ = p16[i] & 0xFF;
			 }
	 } else if (output_pixel_depth == RGB332) {
	   // Load the 8-bit pixel data: RGB332
			 lv_color16_t *p16 = (lv_color16_t *) color_map;
			 for (i=0; i<size; i++) {
		 *buf++ = ((p16[i].red&0x1C)<<3) | ((p16[i].green&0x38)>>1) | ((p16[i].blue&0x18)>>3);
			 }
	 } else if (output_pixel_depth == RGB121) {
	   lv_color16_t *p16 = (lv_color16_t *) color_map;
			 for (i=0; i<size; i+=2) {
		 *buf++ = ((p16[i].red&0x10)<<3) | ((p16[i].green&0x30)<<1) | (p16[i].blue&0x10) |
				  ((p16[i+1].red&0x10)>>1) | ((p16[i+1].green&0x30)>>3) | ((p16[i+1].blue&0x10)>>4);
	   }
	 } else if (output_pixel_depth == MONO4) {
	   lv_color16_t *p16 = (lv_color16_t *) color_map;
	   for (i=0; i<size; i++) {
		 uint8_t pix;
		 pix = (uint8_t)((p16[i].red*0.2126f/2) + (p16[i].green*0.7152f/4) + (p16[i].blue*0.0722f/2));
		 if((i&0x01)==0)
		   *buf = ((pix&0x0F)<<4);
		 else
		   *buf++ += (pix&0x0F);
	   }
	 } else if (output_pixel_depth == MONO2) {
	   lv_color16_t *p16 = (lv_color16_t *) color_map;
			 for (i=0; i<size; i++) {
		 uint8_t pix;
		 pix = (uint8_t)((p16[i].red*0.2126f/8) + (p16[i].green*0.7152f/16) + (p16[i].blue*0.0722f/8));
		 switch(i&0x03) {
		   case 0x00:
			 *buf = ((pix&0x03)<<6); break;
		   case 0x01:
			 *buf += ((pix&0x03)<<4); break;
		   case 0x02:
			 *buf += ((pix&0x03)<<2); break;
		   case 0x03:
			 *buf++ += (pix&0x03); break;
		   default: break;
		 }
	   }
	 }
 
	 // Send the buffer to the web page for display
		 i = ws_send_nocopy_bin_all(output_size + PIXEL_BUF_HEADER_LEN + hdr_len);
	 }
   
	 lv_disp_flush_ready(drv);
 }
 
 
 bool websocket_driver_read(lv_indev_t * drv, lv_indev_data_t * data)
 {
	 xSemaphoreTake(pointer.mutex, portMAX_DELAY);
	 data->point.x = (int16_t) pointer.x;
	 data->point.y = (int16_t) pointer.y;
	 data->state = (pointer.flag == 0) ? LV_INDEV_STATE_REL : LV_INDEV_STATE_PR;
	 xSemaphoreGive(pointer.mutex);
	 
	 return false;
 }
 
 
 /**********************
  *   STATIC FUNCTIONS
  **********************/
 // handles websocket events
 void websocket_callback(uint8_t num, WEBSOCKET_TYPE_t type, char* msg, uint64_t len) {
	 const static char* TAG = "websocket_callback";
 
	 switch(type) {
		 case WEBSOCKET_CONNECT:
			 ESP_LOGI(TAG, "client %i connected!", num);
			 websocket_connected = true;
			 // Force a redraw of the screen for the new client
			 lv_obj_invalidate(lv_disp_get_scr_act(lv_disp_get_default()));
			 break;
		 case WEBSOCKET_DISCONNECT_EXTERNAL:
			 ESP_LOGI(TAG, "client %i sent a disconnect message", num);
			 if (num_connected_clients() == 0) {
				 websocket_connected = false;
			 }
			 break;
		 case WEBSOCKET_DISCONNECT_INTERNAL:
			 ESP_LOGI(TAG, "client %i was disconnected", num);
			 if (num_connected_clients() == 0) {
				 websocket_connected = false;
			 }
			 break;
		 case WEBSOCKET_DISCONNECT_ERROR:
			 ESP_LOGI(TAG, "client %i was disconnected due to an error", num);
			 if (num_connected_clients() == 0) {
				 websocket_connected = false;
			 }
			 break;
		 case WEBSOCKET_BIN:
			 if ((uint32_t) len == 5) {
				 xSemaphoreTake(pointer.mutex, portMAX_DELAY);
				 pointer.flag = (uint8_t) msg[0];
				 pointer.x = ((uint8_t) msg[1] << 8) | (uint8_t) msg[2];
				 pointer.y = ((uint8_t) msg[3] << 8) | (uint8_t) msg[4];
				 xSemaphoreGive(pointer.mutex);
			 }
			 break;
		 default:
			 ESP_LOGI(TAG, "client %i send unhandled websocket type %d", num, type);
			 break;
	 }
 }
 
 // serves any clients
 static void http_serve(struct netconn *conn) {
	 const static char* TAG = "http_server";
	 const static char HTML_HEADER[] = "HTTP/1.1 200 OK\nContent-type: text/html\n\n";
	 const static char ICO_HEADER[] = "HTTP/1.1 200 OK\nContent-type: image/x-icon\n\n";
 
	 struct netbuf* inbuf;
	 static char* buf;
	 static uint16_t buflen;
	 static err_t err;
 
	 // default page
	 extern const uint8_t index_html_start[] asm("_binary_index_html_start");
	 extern const uint8_t index_html_end[] asm("_binary_index_html_end");
	 const uint32_t index_html_len = index_html_end - index_html_start;
	 
	 // favicon.ico (automatically requested by browsers from the "root directory")
	 extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
	 extern const uint8_t favicon_ico_end[] asm("_binary_favicon_ico_end");
	 const uint32_t favicon_ico_len = favicon_ico_end - favicon_ico_start;
 
	 netconn_set_recvtimeout(conn,1000); // allow a connection timeout of 1 second
	 ESP_LOGI(TAG, "reading from client...");
	 err = netconn_recv(conn, &inbuf);
	 ESP_LOGI(TAG, "read from client");
	 if(err == ERR_OK) {
		 netbuf_data(inbuf, (void**)&buf, &buflen);
		 if (buf) {
			 // default page
			 if (strstr(buf, "GET / ")
				 && !strstr(buf, "Upgrade: websocket")) {
				 
				 ESP_LOGI(TAG, "Sending /");
				 netconn_write(conn, HTML_HEADER, sizeof(HTML_HEADER)-1, NETCONN_NOCOPY);
				 netconn_write(conn, index_html_start, index_html_len, NETCONN_NOCOPY);
				 netconn_close(conn);
				 netconn_delete(conn);
				 netbuf_delete(inbuf);
			 }
 
			 // default page websocket
			 else if (strstr(buf, "GET / ")
					  && strstr(buf, "Upgrade: websocket")) {
				 ESP_LOGI(TAG, "Requesting websocket on /");
				 ws_server_add_client(conn, buf, buflen, (char *)"/", websocket_callback);
				 netbuf_delete(inbuf);
			 }
			 
			 else if(strstr(buf,"GET /favicon.ico ")) {
				 ESP_LOGI(TAG, "Sending favicon.ico");
				 netconn_write(conn,ICO_HEADER, sizeof(ICO_HEADER)-1, NETCONN_NOCOPY);
				 netconn_write(conn,favicon_ico_start, favicon_ico_len, NETCONN_NOCOPY);
				 netconn_close(conn);
				 netconn_delete(conn);
				 netbuf_delete(inbuf);
			 }
 
			 else {
				 ESP_LOGI(TAG, "Unknown request");
				 netconn_close(conn);
				 netconn_delete(conn);
				 netbuf_delete(inbuf);
			 }
		 }
		 else {
			 ESP_LOGI(TAG, "Unknown request (empty?...)");
			 netconn_close(conn);
			 netconn_delete(conn);
			 netbuf_delete(inbuf);
		 }
	 }
	 else {
		 ESP_LOGE(TAG, "error on read, closing connection");
		 netconn_close(conn);
		 netconn_delete(conn);
		 netbuf_delete(inbuf);
	 }
 }
 
 // handles clients when they first connect. passes to a queue
 static void server_task(void* pvParameters) {
	 const static char* TAG = "server_task";
	 struct netconn *conn, *newconn;
	 static err_t err;
	 client_queue = xQueueCreate(client_queue_size, sizeof(struct netconn*));
 
	 conn = netconn_new(NETCONN_TCP);
	 netconn_bind(conn, NULL, 80);
	 netconn_listen(conn);
	 ESP_LOGI(TAG,"server listening");
	 do {
		 err = netconn_accept(conn, &newconn);
		 ESP_LOGI(TAG, "new client");
		 if(err == ERR_OK) {
			 xQueueSendToBack(client_queue, &newconn, portMAX_DELAY);
			 //http_serve(newconn);
		 }
	 } while(err == ERR_OK);
	 netconn_close(conn);
	 netconn_delete(conn);
	 ESP_LOGE(TAG,"task ending, rebooting board");
	 esp_restart();
 }
 
 // receives clients from queue, handles them
 static void server_handle_task(void* pvParameters) {
	 const static char* TAG = "server_handle_task";
	 struct netconn* conn;
	 ESP_LOGI(TAG, "task starting");
	 for(;;) {
		 xQueueReceive(client_queue, &conn,portMAX_DELAY);
		 if (!conn) continue;
		 http_serve(conn);
	 }
	 vTaskDelete(NULL);
 }
 
 // Setup the websocket header, return the first position for payload
 // Note: We do not encrypt using the mask because that would slow us down...
 static int set_msg_buf(uint32_t msg_len)
 {
	 int pos;
	 ws_header_t header;
	 
	 header.param.pos.ZERO = 0; // reset the whole header
	 header.param.pos.ONE  = 0;
 
	 header.param.bit.FIN = 1; // all pieces are done (assume we aren't sending that huge of a message)
	 header.param.bit.OPCODE = WEBSOCKET_OPCODE_BIN;
	 
	 // populate LEN field
	 pos = 2;
	 header.length = (uint64_t) msg_len;
	 if (msg_len <= 125) {
		 header.param.bit.LEN = msg_len;
	 }
	 else if (msg_len < 65536) {
		 header.param.bit.LEN = 126;
		 pos += 2;
	 }
	 else {
		 header.param.bit.LEN = 127;
		 pos += 8;
	 }
	 
	 // Setup the websocket header bytes
	 msg_buf[0] = header.param.pos.ZERO;
	 msg_buf[1] = header.param.pos.ONE;
	 // put in the length, if necessary
	 if (header.param.bit.LEN == 126) {
		 msg_buf[2] = (msg_len >> 8) & 0xFF;
		 msg_buf[3] = (msg_len     ) & 0xFF;
	 }
	 if (header.param.bit.LEN == 127) {
		 msg_buf[2] = 0;
		 msg_buf[3] = 0;
		 msg_buf[4] = 0;
		 msg_buf[5] = 0;
		 msg_buf[6] = (msg_len >> 24) & 0xFF;
		 msg_buf[7] = (msg_len >> 16) & 0xFF;
		 msg_buf[8] = (msg_len >> 8)  & 0xFF;
		 msg_buf[9] = (msg_len)       & 0xFF;
	   }
	   
	   return pos;
 }
 
 // Send our msg_buf out the websocket to all connected clients using nocopy
 // This function is an optimized version of the websocket_server's 
 // ws_server_send_bin_all() function eliminating a bunch of data copying.
 // Returns number of clients written.
 static int ws_send_nocopy_bin_all(uint32_t len)
 {
	 int ret = 0;
	 int err;
   
	 // Grab the websocket server's mutex
	 xSemaphoreTake(xwebsocket_mutex,portMAX_DELAY);
	 
	 // Send to all connected clients
	 for (int i=0; i<WEBSOCKET_SERVER_MAX_CLIENTS; i++) {
		 if (ws_is_connected(clients[i])) {
			 err = netconn_write(clients[i].conn, msg_buf, len, NETCONN_NOCOPY);
			 if (!err) {
				 ret += 1;
			 } else {
				 clients[i].scallback(i, WEBSOCKET_DISCONNECT_ERROR, NULL, 0);
				 ws_disconnect_client(&clients[i], 0);
			 }
		 }
	 }
	 
	 xSemaphoreGive(xwebsocket_mutex);
	 
	 return ret;
 }
 
 static int num_connected_clients()
 {
	 int ret = 0;
	 
	 for (int i=0; i<WEBSOCKET_SERVER_MAX_CLIENTS; i++) {
		 if (ws_is_connected(clients[i])) ret++;
	 }
	 
	 return ret;
 }
 