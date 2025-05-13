#include "esp_camera.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#if DEVICE == 0 // esp32s3mini
  #define PWDN_GPIO_NUM     9
  #define RESET_GPIO_NUM    10
  #define XCLK_GPIO_NUM     11
  #define SIOD_GPIO_NUM     21
  #define SIOC_GPIO_NUM     17

  #define Y9_GPIO_NUM       5
  #define Y8_GPIO_NUM       6
  #define Y7_GPIO_NUM       7
  #define Y6_GPIO_NUM       8
  #define Y5_GPIO_NUM       13
  #define Y4_GPIO_NUM       14
  #define Y3_GPIO_NUM       16
  #define Y2_GPIO_NUM       15
  #define VSYNC_GPIO_NUM    39
  #define HREF_GPIO_NUM     36
  #define PCLK_GPIO_NUM     40
#define PIN_BTN GPIO_NUM_0
#define PIN_LED GPIO_NUM_2
#elif DEVICE == 1 // esp32s2fh2
  #define PWDN_GPIO_NUM     2
  #define RESET_GPIO_NUM    1
  #define XCLK_GPIO_NUM     11
  #define SIOD_GPIO_NUM     21
  #define SIOC_GPIO_NUM     17

  #define Y9_GPIO_NUM       3
  #define Y8_GPIO_NUM       4
  #define Y7_GPIO_NUM       5
  #define Y6_GPIO_NUM       6
  #define Y5_GPIO_NUM       7
  #define Y4_GPIO_NUM       8
  #define Y3_GPIO_NUM       14
  #define Y2_GPIO_NUM       12
  #define VSYNC_GPIO_NUM    39
  #define HREF_GPIO_NUM     10
  #define PCLK_GPIO_NUM     13
#elif DEVICE == 2 // esp32cam
  #define PWDN_GPIO_NUM     32
  #define RESET_GPIO_NUM    -1
  #define XCLK_GPIO_NUM     0
  #define SIOD_GPIO_NUM     26
  #define SIOC_GPIO_NUM     27

  #define Y9_GPIO_NUM       35
  #define Y8_GPIO_NUM       34
  #define Y7_GPIO_NUM       39
  #define Y6_GPIO_NUM       36
  #define Y5_GPIO_NUM       21
  #define Y4_GPIO_NUM       19
  #define Y3_GPIO_NUM       18
  #define Y2_GPIO_NUM       5
  #define VSYNC_GPIO_NUM    25
  #define HREF_GPIO_NUM     23
  #define PCLK_GPIO_NUM     22
#endif

struct spijpeg_pack{
  int32_t size;
  uint8_t bytes[4000];
};

constexpr int jpegbuf_size = sizeof(spijpeg_pack);

enum spi_msg{
  MSG_OK = 0x01,
  MSG_REQUEST = 0x02,
  MSG_CHG_PARAM = 0x03,
  MSG_ERR = 0xFF,
  MSG_LEN_RECEIVED = 0x04,
};

static camera_config_t camera_config = {
    .pin_pwdn = -1,//PWDN_GPIO_NUM;
    .pin_reset = RESET_GPIO_NUM,
    .pin_xclk = XCLK_GPIO_NUM,
    .pin_sccb_sda = SIOD_GPIO_NUM,
    .pin_sccb_scl = SIOC_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM,
    .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href = HREF_GPIO_NUM,
    .pin_pclk = PCLK_GPIO_NUM,
    .xclk_freq_hz = 40000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_GRAYSCALE,
    .frame_size = FRAMESIZE_QQVGA,
    .jpeg_quality = 10,
    .fb_count = 2,
    .fb_location = CAMERA_FB_IN_DRAM,
    .grab_mode = CAMERA_GRAB_LATEST
  };
constexpr int QQVGA_FRAME_SIZE = 160 * 120;
constexpr int QCIF_FRAME_SIZE = 176 * 144;
constexpr int QVGA_FRAME_SIZE = 320 * 240;
jpeg_enc_config_t jpeg_enc_cfg = {
  .width = 176,
  .height = 144,
  .src_type = JPEG_PIXEL_FORMAT_GRAY,
  .subsampling = JPEG_SUBSAMPLE_GRAY,
  .quality = 40,
  .rotate = JPEG_ROTATE_0D,
  .task_enable = false,
  .hfm_task_priority = 13,
  .hfm_task_core = 1,
};
esp_err_t initCam(){
  // camera_config.fb_location = CAMERA_FB_IN_PSRAM;
  return esp_camera_init(&camera_config);
}
jpeg_pixel_format_t convertPixFormat(pixformat_t srcfmt){
  switch(srcfmt){
    case PIXFORMAT_GRAYSCALE:
      return JPEG_PIXEL_FORMAT_GRAY;
    case PIXFORMAT_RGB888:
      return JPEG_PIXEL_FORMAT_RGB888;
    case PIXFORMAT_RGB565:
      return JPEG_PIXEL_FORMAT_RGB565_LE;
    case PIXFORMAT_YUV422:
      return JPEG_PIXEL_FORMAT_YCbYCr;
    case PIXFORMAT_YUV420:
      return JPEG_PIXEL_FORMAT_YCbY2YCrY2;
    default:
      return JPEG_PIXEL_FORMAT_GRAY;
  }
}

//frame2jpg as esp_new_jpeg
bool newFrame2jpg(camera_fb_t *fb, uint8_t *out_buf, int *out_len){


    jpeg_error_t ret = JPEG_ERR_OK;
    constexpr int image_size = QVGA_FRAME_SIZE * 3;
    static int block_size = 0;
    static int num_times = 0;
    jpeg_enc_handle_t jpeg_enc = NULL;

    // Serial.println("Opening JPEG Encoder");
    // open
    jpeg_enc_cfg.width = fb->width;
    jpeg_enc_cfg.height = fb->height;
    jpeg_enc_cfg.src_type = convertPixFormat(fb->format);
    ret = jpeg_enc_open(&jpeg_enc_cfg, &jpeg_enc);
    uint8_t i;
    for (i=0;ret != JPEG_ERR_OK && i<5;i++) {
      ESP_LOGE("CAM","JPEG Encoder Open Failed - Reason : %d\n",ret);
      ret = jpeg_enc_open(&jpeg_enc_cfg, &jpeg_enc);
    }
    if(i==5){
      ESP_LOGE("CAM","JPEG Encoder Open Failed");
      return false;
    }
    // Serial.println("JPEG Encoder Opened");

    // process
    block_size = jpeg_enc_get_block_size(jpeg_enc);
    num_times = image_size / block_size;
    int in_offset = 0;
    // Serial.printf("Processing JPEG : Block Size : %d, Num Times : %d\n",block_size,num_times);

    for (size_t j = 0; j < num_times; j++) {
        // Serial.printf("Processing Block %d",j);
        // copy memory or read from SDCard
        ret = jpeg_enc_process_with_block(jpeg_enc, fb->buf + in_offset, block_size, out_buf, jpegbuf_size-4, out_len);
        if (ret <= JPEG_ERR_OK) {
          jpeg_enc_close(jpeg_enc);
          // if (*out_buf) {
          //     free(*out_buf);
          // }
          // Serial.printf("Output Buffer Freed\n");

          if(ret != JPEG_ERR_OK){
            ESP_LOGE("CAM","JPEG Encoder Process Failed - Reason : %d\n",ret);
            return false;
          }
          return true;
        }
        in_offset += block_size;
    }
    ESP_LOGE("CAM","JPEG process overrunned");
    jpeg_enc_close(jpeg_enc);
    return false;
}
esp_err_t getCameraJpeg(uint8_t *out_buf, int *out_len){
    camera_fb_t * fb = NULL;
    // uint8_t *new_buf = NULL;
    // ESP_LOGI("CAM","free heap size : %lu\n",esp_get_free_heap_size());
    // new_buf = (uint8_t*)malloc(jpegbuf_size);
    // if(!new_buf){
    //     ESP_LOGE("CAM","JPEG buffer malloc failed");
    //     return ESP_FAIL;
    // }
    //take a picture with camera
    fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE("CAM","Camera capture failed");
        return ESP_FAIL;
    }
    //log frame buffer info
    // ESP_LOGI("CAM","Frame Buffer Info : %d %d %d %d",fb->width,fb->height,fb->len,fb->format);
    if(fb->format != PIXFORMAT_JPEG){
        bool jpeg_converted = newFrame2jpg(fb, out_buf, out_len);
        if(!jpeg_converted){
            ESP_LOGE("CAM","JPEG compression failed");
            esp_camera_fb_return(fb);
            return ESP_FAIL;
        }
        // memcpy(out_buf, new_buf, *out_len);
        // ESP_LOGI("CAM","JPEG buffer copied %d",*out_len);
        //release new_buf
    }
    else{
        *out_len = fb->len;
    }
    esp_camera_fb_return(fb);
    return ESP_OK;
}