#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <jpeglib.h>
#include <linux/spi/spidev.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_system.h>

#include "camera_common.h"
#include "t23_border_pipeline.h"
#include "t23_c3_protocol.h"

#define UART_DEFAULT_DEVICE "/dev/ttyS0"
#define UART_DEFAULT_BAUD B115200
#define UART_RX_BUF_SIZE 512
#define JPEG_MAX_SIZE (512 * 1024)
#define RECTIFIED_JPEG_QUALITY 75
#define RECTIFIED_MAX_WIDTH 640
#define RECTIFIED_MAX_HEIGHT 360

#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_MODE_DEFAULT SPI_MODE_0
#define SPI_BITS_DEFAULT 8
#define SPI_SPEED_DEFAULT 20000000U
#define DATA_READY_GPIO 53

extern struct chn_conf chn[];

typedef struct {
    const char *serial_device;
    speed_t baud_rate;
} bridge_cfg_t;

typedef struct {
    const char *name;
    uint8_t param_id;
    int (*get_value)(int *value);
    int (*set_value)(int value);
    int min_value;
    int max_value;
} bridge_param_desc_t;

typedef struct {
    double x;
    double y;
} pointf_t;

typedef enum {
    BRIDGE_MODE_DEBUG = 0,
    BRIDGE_MODE_RUN = 1,
} bridge_mode_t;

typedef struct {
    t23_border_layout_t layout;
    const char *name;
    unsigned int top_blocks;
    unsigned int right_blocks;
    unsigned int bottom_blocks;
    unsigned int left_blocks;
} border_layout_desc_t;

static volatile sig_atomic_t g_running = 1;
static unsigned char g_jpeg_buf[JPEG_MAX_SIZE];
static int g_jpeg_chn_id = -1;
static int g_jpeg_recv_started = 0;
static int g_spi_fd = -1;
static int g_data_ready_fd = -1;
static t23_border_calibration_t g_calibration;
static bridge_mode_t g_bridge_mode = BRIDGE_MODE_DEBUG;
static t23_border_layout_t g_border_layout = T23_BORDER_LAYOUT_16X9;

static void send_line(int fd, const char *line);
static void sendf(int fd, const char *fmt, ...);
static int capture_jpeg_once(unsigned char *out_buf, size_t *out_len);
static int push_jpeg_over_spi(const unsigned char *jpeg_buf, size_t jpeg_len);

static const char *bridge_mode_name(bridge_mode_t mode)
{
    return (mode == BRIDGE_MODE_RUN) ? "RUN" : "DEBUG";
}

static const border_layout_desc_t *get_border_layout_desc(t23_border_layout_t layout)
{
    static const border_layout_desc_t layout_16x9 = {
        T23_BORDER_LAYOUT_16X9, "16X9",
        T23_BORDER_LAYOUT_16X9_TOP,
        T23_BORDER_LAYOUT_16X9_RIGHT,
        T23_BORDER_LAYOUT_16X9_BOTTOM,
        T23_BORDER_LAYOUT_16X9_LEFT
    };
    static const border_layout_desc_t layout_4x3 = {
        T23_BORDER_LAYOUT_4X3, "4X3",
        T23_BORDER_LAYOUT_4X3_TOP,
        T23_BORDER_LAYOUT_4X3_RIGHT,
        T23_BORDER_LAYOUT_4X3_BOTTOM,
        T23_BORDER_LAYOUT_4X3_LEFT
    };

    if (layout == T23_BORDER_LAYOUT_4X3) {
        return &layout_4x3;
    }
    return &layout_16x9;
}

static unsigned int border_layout_block_count(const border_layout_desc_t *layout)
{
    return layout->top_blocks + layout->right_blocks + layout->bottom_blocks + layout->left_blocks;
}

static void send_mode_status(int fd)
{
    sendf(fd, "MODE %s", bridge_mode_name(g_bridge_mode));
}

static void send_layout_status(int fd)
{
    const border_layout_desc_t *layout = get_border_layout_desc(g_border_layout);

    sendf(fd,
          "LAYOUT %s %u %u %u %u %u",
          layout->name,
          border_layout_block_count(layout),
          layout->top_blocks,
          layout->right_blocks,
          layout->bottom_blocks,
          layout->left_blocks);
}

static void usage(const char *prog)
{
    printf("Usage: %s [--port /dev/ttyS0] [--baud 115200]\n", prog);
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static sample_sensor_cfg_t make_sensor_cfg(void)
{
    sample_sensor_cfg_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.sensor_name, sizeof(cfg.sensor_name), "%s", SENSOR_NAME);
    cfg.sensor_cubs_type = SENSOR_CUBS_TYPE;
    cfg.sensor_i2c_addr = SENSOR_I2C_ADDR;
    cfg.sensor_i2c_adapter_id = SENSOR_I2C_ADAPTER_ID;
    cfg.sensor_width = SENSOR_WIDTH;
    cfg.sensor_height = SENSOR_HEIGHT;
    cfg.chn0_en = CHN0_EN;
    cfg.chn1_en = CHN1_EN;
    cfg.chn2_en = CHN2_EN;
    cfg.chn3_en = CHN3_EN;
    cfg.crop_en = CROP_EN;

    return cfg;
}

static void init_default_calibration(void)
{
    int w = SENSOR_WIDTH;
    int h = SENSOR_HEIGHT;
    int left = w / 8;
    int right = w - left;
    int top = h / 8;
    int bottom = h - top;
    int cx = w / 2;
    int cy = h / 2;

    memset(&g_calibration, 0, sizeof(g_calibration));
    g_calibration.image_width = (uint16_t)w;
    g_calibration.image_height = (uint16_t)h;

    g_calibration.points[T23_BORDER_POINT_TL].x = (int16_t)left;
    g_calibration.points[T23_BORDER_POINT_TL].y = (int16_t)top;
    g_calibration.points[T23_BORDER_POINT_TM].x = (int16_t)cx;
    g_calibration.points[T23_BORDER_POINT_TM].y = (int16_t)top;
    g_calibration.points[T23_BORDER_POINT_TR].x = (int16_t)right;
    g_calibration.points[T23_BORDER_POINT_TR].y = (int16_t)top;
    g_calibration.points[T23_BORDER_POINT_RM].x = (int16_t)right;
    g_calibration.points[T23_BORDER_POINT_RM].y = (int16_t)cy;
    g_calibration.points[T23_BORDER_POINT_BR].x = (int16_t)right;
    g_calibration.points[T23_BORDER_POINT_BR].y = (int16_t)bottom;
    g_calibration.points[T23_BORDER_POINT_BM].x = (int16_t)cx;
    g_calibration.points[T23_BORDER_POINT_BM].y = (int16_t)bottom;
    g_calibration.points[T23_BORDER_POINT_BL].x = (int16_t)left;
    g_calibration.points[T23_BORDER_POINT_BL].y = (int16_t)bottom;
    g_calibration.points[T23_BORDER_POINT_LM].x = (int16_t)left;
    g_calibration.points[T23_BORDER_POINT_LM].y = (int16_t)cy;
}

static void sanitize_calibration(t23_border_calibration_t *cal)
{
    unsigned int i;
    int max_x = cal->image_width > 0 ? (int)cal->image_width - 1 : 0;
    int max_y = cal->image_height > 0 ? (int)cal->image_height - 1 : 0;

    if (cal->image_width == 0) {
        cal->image_width = SENSOR_WIDTH;
    }
    if (cal->image_height == 0) {
        cal->image_height = SENSOR_HEIGHT;
    }

    max_x = (int)cal->image_width - 1;
    max_y = (int)cal->image_height - 1;

    for (i = 0; i < T23_BORDER_POINT_COUNT; ++i) {
        cal->points[i].x = (int16_t)clamp_int(cal->points[i].x, 0, max_x);
        cal->points[i].y = (int16_t)clamp_int(cal->points[i].y, 0, max_y);
    }
}

static double pointf_distance(pointf_t a, pointf_t b)
{
    double dx = a.x - b.x;
    double dy = a.y - b.y;

    return sqrt(dx * dx + dy * dy);
}

static pointf_t pointf_bezier2(pointf_t p0, pointf_t p1, pointf_t p2, double t)
{
    double omt = 1.0 - t;
    pointf_t out;

    out.x = omt * omt * p0.x + 2.0 * omt * t * p1.x + t * t * p2.x;
    out.y = omt * omt * p0.y + 2.0 * omt * t * p1.y + t * t * p2.y;
    return out;
}

static void scaled_calibration_points(const t23_border_calibration_t *cal,
                                      int src_w,
                                      int src_h,
                                      pointf_t pts[T23_BORDER_POINT_COUNT])
{
    double sx = (cal->image_width > 0) ? (double)src_w / (double)cal->image_width : 1.0;
    double sy = (cal->image_height > 0) ? (double)src_h / (double)cal->image_height : 1.0;
    unsigned int i;

    for (i = 0; i < T23_BORDER_POINT_COUNT; ++i) {
        pts[i].x = cal->points[i].x * sx;
        pts[i].y = cal->points[i].y * sy;
    }
}

static void compute_rectified_size(const pointf_t pts[T23_BORDER_POINT_COUNT], int *out_w, int *out_h)
{
    double top_len = pointf_distance(pts[T23_BORDER_POINT_TL], pts[T23_BORDER_POINT_TM]) +
                     pointf_distance(pts[T23_BORDER_POINT_TM], pts[T23_BORDER_POINT_TR]);
    double bottom_len = pointf_distance(pts[T23_BORDER_POINT_BL], pts[T23_BORDER_POINT_BM]) +
                        pointf_distance(pts[T23_BORDER_POINT_BM], pts[T23_BORDER_POINT_BR]);
    double left_len = pointf_distance(pts[T23_BORDER_POINT_TL], pts[T23_BORDER_POINT_LM]) +
                      pointf_distance(pts[T23_BORDER_POINT_LM], pts[T23_BORDER_POINT_BL]);
    double right_len = pointf_distance(pts[T23_BORDER_POINT_TR], pts[T23_BORDER_POINT_RM]) +
                       pointf_distance(pts[T23_BORDER_POINT_RM], pts[T23_BORDER_POINT_BR]);
    double width_est = (top_len + bottom_len) * 0.5;
    double height_est = (left_len + right_len) * 0.5;
    int width = (int)lrint(width_est);
    int height = (int)lrint(height_est);

    width = clamp_int(width, 160, RECTIFIED_MAX_WIDTH);
    height = (int)lrint((double)width * 9.0 / 16.0);

    if (height > RECTIFIED_MAX_HEIGHT || height > (int)height_est) {
        height = clamp_int((int)lrint(height_est), 90, RECTIFIED_MAX_HEIGHT);
        width = (int)lrint((double)height * 16.0 / 9.0);
    }

    width = clamp_int(width, 160, RECTIFIED_MAX_WIDTH);
    height = clamp_int(height, 90, RECTIFIED_MAX_HEIGHT);

    *out_w = width;
    *out_h = height;
}

static void compute_calibration_bbox(const pointf_t pts[T23_BORDER_POINT_COUNT],
                                     int src_w,
                                     int src_h,
                                     int *left_out,
                                     int *top_out,
                                     int *right_out,
                                     int *bottom_out)
{
    double min_x = pts[0].x;
    double max_x = pts[0].x;
    double min_y = pts[0].y;
    double max_y = pts[0].y;
    unsigned int i;

    for (i = 1; i < T23_BORDER_POINT_COUNT; ++i) {
        if (pts[i].x < min_x) {
            min_x = pts[i].x;
        }
        if (pts[i].x > max_x) {
            max_x = pts[i].x;
        }
        if (pts[i].y < min_y) {
            min_y = pts[i].y;
        }
        if (pts[i].y > max_y) {
            max_y = pts[i].y;
        }
    }

    *left_out = clamp_int((int)floor(min_x), 0, src_w - 1);
    *top_out = clamp_int((int)floor(min_y), 0, src_h - 1);
    *right_out = clamp_int((int)ceil(max_x), 0, src_w - 1);
    *bottom_out = clamp_int((int)ceil(max_y), 0, src_h - 1);

    if (*right_out <= *left_out) {
        *right_out = clamp_int(*left_out + 1, 1, src_w - 1);
    }
    if (*bottom_out <= *top_out) {
        *bottom_out = clamp_int(*top_out + 1, 1, src_h - 1);
    }
}

static pointf_t coons_patch_point(const pointf_t pts[T23_BORDER_POINT_COUNT], double u, double v)
{
    pointf_t top = pointf_bezier2(pts[T23_BORDER_POINT_TL], pts[T23_BORDER_POINT_TM], pts[T23_BORDER_POINT_TR], u);
    pointf_t bottom = pointf_bezier2(pts[T23_BORDER_POINT_BL], pts[T23_BORDER_POINT_BM], pts[T23_BORDER_POINT_BR], u);
    pointf_t left = pointf_bezier2(pts[T23_BORDER_POINT_TL], pts[T23_BORDER_POINT_LM], pts[T23_BORDER_POINT_BL], v);
    pointf_t right = pointf_bezier2(pts[T23_BORDER_POINT_TR], pts[T23_BORDER_POINT_RM], pts[T23_BORDER_POINT_BR], v);
    pointf_t bilinear;
    pointf_t out;
    double omt_u = 1.0 - u;
    double omt_v = 1.0 - v;

    bilinear.x = omt_u * omt_v * pts[T23_BORDER_POINT_TL].x +
                 u * omt_v * pts[T23_BORDER_POINT_TR].x +
                 omt_u * v * pts[T23_BORDER_POINT_BL].x +
                 u * v * pts[T23_BORDER_POINT_BR].x;
    bilinear.y = omt_u * omt_v * pts[T23_BORDER_POINT_TL].y +
                 u * omt_v * pts[T23_BORDER_POINT_TR].y +
                 omt_u * v * pts[T23_BORDER_POINT_BL].y +
                 u * v * pts[T23_BORDER_POINT_BR].y;

    out.x = omt_u * left.x + u * right.x + omt_v * top.x + v * bottom.x - bilinear.x;
    out.y = omt_u * left.y + u * right.y + omt_v * top.y + v * bottom.y - bilinear.y;
    return out;
}

static unsigned char bilinear_sample_channel(const unsigned char *rgb,
                                             int src_w,
                                             int src_h,
                                             double x,
                                             double y,
                                             int channel)
{
    int x0;
    int y0;
    int x1;
    int y1;
    double fx;
    double fy;
    int idx00;
    int idx10;
    int idx01;
    int idx11;
    double v00;
    double v10;
    double v01;
    double v11;
    double top;
    double bottom;
    double value;

    if (x < 0.0 || y < 0.0 || x > (double)(src_w - 1) || y > (double)(src_h - 1)) {
        return 0;
    }

    x0 = (int)floor(x);
    y0 = (int)floor(y);
    x1 = (x0 + 1 < src_w) ? (x0 + 1) : x0;
    y1 = (y0 + 1 < src_h) ? (y0 + 1) : y0;
    fx = x - x0;
    fy = y - y0;

    idx00 = (y0 * src_w + x0) * 3 + channel;
    idx10 = (y0 * src_w + x1) * 3 + channel;
    idx01 = (y1 * src_w + x0) * 3 + channel;
    idx11 = (y1 * src_w + x1) * 3 + channel;

    v00 = rgb[idx00];
    v10 = rgb[idx10];
    v01 = rgb[idx01];
    v11 = rgb[idx11];

    top = v00 + (v10 - v00) * fx;
    bottom = v01 + (v11 - v01) * fx;
    value = top + (bottom - top) * fy;

    return (unsigned char)clamp_int((int)lrint(value), 0, 255);
}

static int decode_jpeg_to_rgb888(const unsigned char *jpeg_data,
                                 unsigned long jpeg_size,
                                 unsigned char **rgb_out,
                                 int *width_out,
                                 int *height_out)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char *rgb;
    int stride;

    *rgb_out = NULL;
    *width_out = 0;
    *height_out = 0;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    if (cinfo.output_width == 0 || cinfo.output_height == 0 || cinfo.output_components != 3) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    stride = (int)cinfo.output_width * 3;
    rgb = malloc((size_t)stride * cinfo.output_height);
    if (rgb == NULL) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *row = rgb + cinfo.output_scanline * stride;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    *width_out = (int)cinfo.output_width;
    *height_out = (int)cinfo.output_height;

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *rgb_out = rgb;
    return 0;
}

static int encode_rgb888_to_jpeg(const unsigned char *rgb,
                                 int width,
                                 int height,
                                 unsigned char **jpeg_out,
                                 size_t *jpeg_size_out)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char *jpeg_mem = NULL;
    size_t jpeg_size = 0;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &jpeg_mem, &jpeg_size);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, RECTIFIED_JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + cinfo.next_scanline * width * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    *jpeg_out = jpeg_mem;
    *jpeg_size_out = jpeg_size;
    return 0;
}

static int build_rectified_rgb_from_calibration(const unsigned char *src_jpeg,
                                                size_t src_jpeg_len,
                                                unsigned char **dst_rgb_out,
                                                int *dst_w_out,
                                                int *dst_h_out,
                                                int *rect_left_out,
                                                int *rect_top_out,
                                                int *rect_right_out,
                                                int *rect_bottom_out)
{
    unsigned char *src_rgb = NULL;
    unsigned char *dst_rgb = NULL;
    pointf_t pts[T23_BORDER_POINT_COUNT];
    int src_w = 0;
    int src_h = 0;
    int dst_w = 0;
    int dst_h = 0;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 0;
    int rect_bottom = 0;
    int x;
    int y;

    *dst_rgb_out = NULL;
    *dst_w_out = 0;
    *dst_h_out = 0;
    *rect_left_out = 0;
    *rect_top_out = 0;
    *rect_right_out = 0;
    *rect_bottom_out = 0;

    if (decode_jpeg_to_rgb888(src_jpeg, (unsigned long)src_jpeg_len, &src_rgb, &src_w, &src_h) < 0) {
        return -1;
    }

    scaled_calibration_points(&g_calibration, src_w, src_h, pts);
    compute_rectified_size(pts, &dst_w, &dst_h);
    dst_rgb = malloc((size_t)dst_w * dst_h * 3);
    if (dst_rgb == NULL) {
        free(src_rgb);
        return -1;
    }
    rect_left = 0;
    rect_top = 0;
    rect_right = dst_w - 1;
    rect_bottom = dst_h - 1;

    for (y = 0; y < dst_h; ++y) {
        for (x = 0; x < dst_w; ++x) {
            double u;
            double v;
            pointf_t src;
            unsigned char *pixel = dst_rgb + (y * dst_w + x) * 3;

            u = (dst_w > 1) ? (double)x / (double)(dst_w - 1) : 0.0;
            v = (dst_h > 1) ? (double)y / (double)(dst_h - 1) : 0.0;
            src = coons_patch_point(pts, u, v);

            pixel[0] = bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 0);
            pixel[1] = bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 1);
            pixel[2] = bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 2);
        }
    }

    free(src_rgb);
    *dst_rgb_out = dst_rgb;
    *dst_w_out = dst_w;
    *dst_h_out = dst_h;
    *rect_left_out = rect_left;
    *rect_top_out = rect_top;
    *rect_right_out = rect_right;
    *rect_bottom_out = rect_bottom;
    return 0;
}

static int rectify_jpeg_from_calibration(const unsigned char *src_jpeg,
                                         size_t src_jpeg_len,
                                         unsigned char *out_jpeg,
                                         size_t out_jpeg_capacity,
                                         size_t *out_jpeg_len)
{
    unsigned char *dst_rgb = NULL;
    unsigned char *jpeg_mem = NULL;
    size_t jpeg_size = 0;
    int dst_w = 0;
    int dst_h = 0;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 0;
    int rect_bottom = 0;

    if (build_rectified_rgb_from_calibration(src_jpeg,
                                             src_jpeg_len,
                                             &dst_rgb,
                                             &dst_w,
                                             &dst_h,
                                             &rect_left,
                                             &rect_top,
                                             &rect_right,
                                             &rect_bottom) < 0) {
        return -1;
    }

    if (encode_rgb888_to_jpeg(dst_rgb, dst_w, dst_h, &jpeg_mem, &jpeg_size) < 0) {
        free(dst_rgb);
        return -1;
    }

    if (jpeg_size == 0 || jpeg_size > out_jpeg_capacity) {
        free(jpeg_mem);
        free(dst_rgb);
        return -1;
    }

    memcpy(out_jpeg, jpeg_mem, jpeg_size);
    *out_jpeg_len = (size_t)jpeg_size;

    free(jpeg_mem);
    free(dst_rgb);
    return 0;
}

static void compute_average_rect(const unsigned char *rgb,
                                 int width,
                                 int height,
                                 int left,
                                 int top,
                                 int right,
                                 int bottom,
                                 t23_rgb8_t *out)
{
    unsigned long long r_sum = 0;
    unsigned long long g_sum = 0;
    unsigned long long b_sum = 0;
    unsigned long long count = 0;
    int x;
    int y;

    left = clamp_int(left, 0, width - 1);
    right = clamp_int(right, 0, width - 1);
    top = clamp_int(top, 0, height - 1);
    bottom = clamp_int(bottom, 0, height - 1);
    if (right < left) {
        right = left;
    }
    if (bottom < top) {
        bottom = top;
    }

    for (y = top; y <= bottom; ++y) {
        for (x = left; x <= right; ++x) {
            const unsigned char *p = rgb + (y * width + x) * 3;

            r_sum += p[0];
            g_sum += p[1];
            b_sum += p[2];
            ++count;
        }
    }

    if (count == 0) {
        out->r = 0;
        out->g = 0;
        out->b = 0;
        return;
    }

    out->r = (uint8_t)(r_sum / count);
    out->g = (uint8_t)(g_sum / count);
    out->b = (uint8_t)(b_sum / count);
}

static int compute_border_blocks_from_calibration(const unsigned char *src_jpeg,
                                                  size_t src_jpeg_len,
                                                  t23_border_layout_t layout_id,
                                                  t23_border_block_t blocks[T23_BORDER_BLOCK_COUNT_MAX],
                                                  unsigned int *block_count_out,
                                                  unsigned int *top_blocks_out,
                                                  unsigned int *right_blocks_out,
                                                  unsigned int *bottom_blocks_out,
                                                  unsigned int *left_blocks_out,
                                                  int *image_w_out,
                                                  int *image_h_out,
                                                  int *rect_left_out,
                                                  int *rect_top_out,
                                                  int *rect_right_out,
                                                  int *rect_bottom_out,
                                                  int *thickness_out)
{
    const border_layout_desc_t *layout = get_border_layout_desc(layout_id);
    unsigned char *dst_rgb = NULL;
    int image_w = 0;
    int image_h = 0;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 0;
    int rect_bottom = 0;
    int rect_w;
    int rect_h;
    int thickness;
    unsigned int idx = 0;
    unsigned int i;

    if (build_rectified_rgb_from_calibration(src_jpeg,
                                             src_jpeg_len,
                                             &dst_rgb,
                                             &image_w,
                                             &image_h,
                                             &rect_left,
                                             &rect_top,
                                             &rect_right,
                                             &rect_bottom) < 0) {
        return -1;
    }

    rect_w = rect_right - rect_left + 1;
    rect_h = rect_bottom - rect_top + 1;
    thickness = clamp_int((rect_w < rect_h ? rect_w : rect_h) / 10, 2, rect_h / 2 > 0 ? rect_h / 2 : 2);

    for (i = 0; i < layout->top_blocks; ++i) {
        int x0 = rect_left + (int)((long long)rect_w * i / layout->top_blocks);
        int x1 = rect_left + (int)((long long)rect_w * (i + 1) / layout->top_blocks) - 1;

        blocks[idx].block_index = (uint8_t)idx;
        compute_average_rect(dst_rgb, image_w, image_h, x0, rect_top, x1, rect_top + thickness - 1, &blocks[idx].color);
        ++idx;
    }

    for (i = 0; i < layout->right_blocks; ++i) {
        int y0 = rect_top + (int)((long long)rect_h * i / layout->right_blocks);
        int y1 = rect_top + (int)((long long)rect_h * (i + 1) / layout->right_blocks) - 1;

        blocks[idx].block_index = (uint8_t)idx;
        compute_average_rect(dst_rgb, image_w, image_h, rect_right - thickness + 1, y0, rect_right, y1, &blocks[idx].color);
        ++idx;
    }

    for (i = 0; i < layout->bottom_blocks; ++i) {
        int x0 = rect_left + (int)((long long)rect_w * (layout->bottom_blocks - 1 - i) / layout->bottom_blocks);
        int x1 = rect_left + (int)((long long)rect_w * (layout->bottom_blocks - i) / layout->bottom_blocks) - 1;

        blocks[idx].block_index = (uint8_t)idx;
        compute_average_rect(dst_rgb, image_w, image_h, x0, rect_bottom - thickness + 1, x1, rect_bottom, &blocks[idx].color);
        ++idx;
    }

    for (i = 0; i < layout->left_blocks; ++i) {
        int y0 = rect_top + (int)((long long)rect_h * (layout->left_blocks - 1 - i) / layout->left_blocks);
        int y1 = rect_top + (int)((long long)rect_h * (layout->left_blocks - i) / layout->left_blocks) - 1;

        blocks[idx].block_index = (uint8_t)idx;
        compute_average_rect(dst_rgb, image_w, image_h, rect_left, y0, rect_left + thickness - 1, y1, &blocks[idx].color);
        ++idx;
    }

    free(dst_rgb);
    *block_count_out = idx;
    *top_blocks_out = layout->top_blocks;
    *right_blocks_out = layout->right_blocks;
    *bottom_blocks_out = layout->bottom_blocks;
    *left_blocks_out = layout->left_blocks;
    *image_w_out = image_w;
    *image_h_out = image_h;
    *rect_left_out = rect_left;
    *rect_top_out = rect_top;
    *rect_right_out = rect_right;
    *rect_bottom_out = rect_bottom;
    *thickness_out = thickness;
    return 0;
}

static void send_border_blocks(int fd,
                               const border_layout_desc_t *layout,
                               const t23_border_block_t blocks[T23_BORDER_BLOCK_COUNT_MAX],
                               unsigned int block_count,
                               int image_w,
                               int image_h,
                               int rect_left,
                               int rect_top,
                               int rect_right,
                               int rect_bottom,
                               int thickness)
{
    unsigned int i;

    sendf(fd, "BLOCKS SIZE %d %d", image_w, image_h);
    sendf(fd,
          "BLOCKS META %s %u %u %u %u %u",
          layout->name,
          block_count,
          layout->top_blocks,
          layout->right_blocks,
          layout->bottom_blocks,
          layout->left_blocks);
    sendf(fd, "BLOCKS RECT %d %d %d %d %d", rect_left, rect_top, rect_right, rect_bottom, thickness);
    for (i = 0; i < block_count; ++i) {
        sendf(fd,
              "BLOCK %u %u %u %u",
              i,
              blocks[i].color.r,
              blocks[i].color.g,
              blocks[i].color.b);
    }
    send_line(fd, "OK BLOCKS GET");
}

static void handle_blocks_get(int fd)
{
    size_t jpeg_len = 0;
    const border_layout_desc_t *layout = get_border_layout_desc(g_border_layout);
    t23_border_block_t blocks[T23_BORDER_BLOCK_COUNT_MAX];
    unsigned int block_count = 0;
    unsigned int top_blocks = 0;
    unsigned int right_blocks = 0;
    unsigned int bottom_blocks = 0;
    unsigned int left_blocks = 0;
    int image_w = 0;
    int image_h = 0;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 0;
    int rect_bottom = 0;
    int thickness = 0;

    if (capture_jpeg_once(g_jpeg_buf, &jpeg_len) < 0) {
        send_line(fd, "ERR BLOCKS_SNAP");
        return;
    }

    if (compute_border_blocks_from_calibration(g_jpeg_buf,
                                               jpeg_len,
                                               g_border_layout,
                                               blocks,
                                               &block_count,
                                               &top_blocks,
                                               &right_blocks,
                                               &bottom_blocks,
                                               &left_blocks,
                                               &image_w,
                                               &image_h,
                                               &rect_left,
                                               &rect_top,
                                               &rect_right,
                                               &rect_bottom,
                                               &thickness) < 0) {
        send_line(fd, "ERR BLOCKS_COMPUTE");
        return;
    }

    (void)top_blocks;
    (void)right_blocks;
    (void)bottom_blocks;
    (void)left_blocks;
    send_border_blocks(fd, layout, blocks, block_count, image_w, image_h, rect_left, rect_top, rect_right, rect_bottom, thickness);
}

static void handle_frame_with_blocks(int fd)
{
    size_t jpeg_len = 0;
    const border_layout_desc_t *layout = get_border_layout_desc(g_border_layout);
    t23_border_block_t blocks[T23_BORDER_BLOCK_COUNT_MAX];
    unsigned int block_count = 0;
    unsigned int top_blocks = 0;
    unsigned int right_blocks = 0;
    unsigned int bottom_blocks = 0;
    unsigned int left_blocks = 0;
    int image_w = 0;
    int image_h = 0;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 0;
    int rect_bottom = 0;
    int thickness = 0;

    if (capture_jpeg_once(g_jpeg_buf, &jpeg_len) < 0) {
        send_line(fd, "ERR FRAME_SNAP");
        return;
    }

    if (compute_border_blocks_from_calibration(g_jpeg_buf,
                                               jpeg_len,
                                               g_border_layout,
                                               blocks,
                                               &block_count,
                                               &top_blocks,
                                               &right_blocks,
                                               &bottom_blocks,
                                               &left_blocks,
                                               &image_w,
                                               &image_h,
                                               &rect_left,
                                               &rect_top,
                                               &rect_right,
                                               &rect_bottom,
                                               &thickness) == 0) {
        (void)top_blocks;
        (void)right_blocks;
        (void)bottom_blocks;
        (void)left_blocks;
        send_border_blocks(fd, layout, blocks, block_count, image_w, image_h, rect_left, rect_top, rect_right, rect_bottom, thickness);
    } else {
        send_line(fd, "ERR FRAME_BLOCKS");
    }

    sendf(fd, "SNAP OK %u", (unsigned int)jpeg_len);
    if (push_jpeg_over_spi(g_jpeg_buf, jpeg_len) < 0) {
        send_line(fd, "ERR FRAME_SPI");
    }
}

static int create_groups(void)
{
    int i;

    for (i = 0; i < FS_CHN_NUM; i++) {
        if (!chn[i].enable) {
            continue;
        }
        if (IMP_Encoder_CreateGroup(chn[i].index) < 0) {
            return -1;
        }
    }

    return 0;
}

static int get_active_jpeg_channel(void)
{
    unsigned int i;

    if (g_jpeg_chn_id >= 0) {
        return g_jpeg_chn_id;
    }

    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            g_jpeg_chn_id = 3 + chn[i].index;
            return g_jpeg_chn_id;
        }
    }

    return -1;
}

static int start_jpeg_recv_once(void)
{
    int chn_id = get_active_jpeg_channel();

    if (chn_id < 0) {
        return -1;
    }
    if (g_jpeg_recv_started) {
        return 0;
    }
    if (IMP_Encoder_StartRecvPic(chn_id) < 0) {
        return -1;
    }
    g_jpeg_recv_started = 1;
    return 0;
}

static void stop_jpeg_recv_once(void)
{
    if (!g_jpeg_recv_started || g_jpeg_chn_id < 0) {
        return;
    }

    IMP_Encoder_StopRecvPic(g_jpeg_chn_id);
    g_jpeg_recv_started = 0;
}

static void destroy_groups(void)
{
    int i;

    for (i = 0; i < FS_CHN_NUM; i++) {
        if (!chn[i].enable) {
            continue;
        }
        IMP_Encoder_DestroyGroup(chn[i].index);
    }
}

static int bind_jpeg_channels(void)
{
    int i;

    for (i = 0; i < FS_CHN_NUM; i++) {
        if (!chn[i].enable) {
            continue;
        }
        if (IMP_System_Bind(&chn[i].framesource_chn, &chn[i].imp_encoder) < 0) {
            return -1;
        }
    }

    return 0;
}

static void unbind_jpeg_channels(void)
{
    int i;

    for (i = 0; i < FS_CHN_NUM; i++) {
        if (!chn[i].enable) {
            continue;
        }
        IMP_System_UnBind(&chn[i].framesource_chn, &chn[i].imp_encoder);
    }
}

static int startup_pipeline(void)
{
    sample_sensor_cfg_t sensor_cfg = make_sensor_cfg();

    if (sample_system_init(sensor_cfg) < 0) {
        return -1;
    }
    if (sample_framesource_init() < 0) {
        sample_system_exit();
        return -1;
    }
    if (create_groups() < 0) {
        sample_framesource_exit();
        sample_system_exit();
        return -1;
    }
    if (sample_jpeg_init() < 0) {
        destroy_groups();
        sample_framesource_exit();
        sample_system_exit();
        return -1;
    }
    if (bind_jpeg_channels() < 0) {
        sample_encoder_exit();
        destroy_groups();
        sample_framesource_exit();
        sample_system_exit();
        return -1;
    }
    if (sample_framesource_streamon() < 0) {
        unbind_jpeg_channels();
        sample_encoder_exit();
        destroy_groups();
        sample_framesource_exit();
        sample_system_exit();
        return -1;
    }
    if (start_jpeg_recv_once() < 0) {
        sample_framesource_streamoff();
        unbind_jpeg_channels();
        sample_encoder_exit();
        destroy_groups();
        sample_framesource_exit();
        sample_system_exit();
        return -1;
    }

    return 0;
}

static void shutdown_pipeline(void)
{
    stop_jpeg_recv_once();
    sample_framesource_streamoff();
    unbind_jpeg_channels();
    sample_encoder_exit();
    destroy_groups();
    sample_framesource_exit();
    sample_system_exit();
}

static void close_transport_resources(void)
{
    if (g_data_ready_fd >= 0) {
        close(g_data_ready_fd);
        g_data_ready_fd = -1;
    }
    if (g_spi_fd >= 0) {
        close(g_spi_fd);
        g_spi_fd = -1;
    }
}

static speed_t parse_baud_rate(int baud)
{
    switch (baud) {
    case 115200:
        return B115200;
    case 230400:
        return B230400;
    case 460800:
        return B460800;
    case 921600:
        return B921600;
    default:
        return UART_DEFAULT_BAUD;
    }
}

static int parse_args(int argc, char *argv[], bridge_cfg_t *cfg)
{
    int i;

    cfg->serial_device = UART_DEFAULT_DEVICE;
    cfg->baud_rate = UART_DEFAULT_BAUD;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg->serial_device = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            cfg->baud_rate = parse_baud_rate(atoi(argv[++i]));
            continue;
        }
        usage(argv[0]);
        return -1;
    }

    return 0;
}

static int open_serial_port(const char *device, speed_t baud_rate)
{
    int fd;
    struct termios tio;

    fd = open(device, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("open serial");
        return -1;
    }

    if (tcgetattr(fd, &tio) < 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_rate);
    cfsetospeed(&tio, baud_rate);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void send_line(int fd, const char *line)
{
    printf("bridge tx: %s\n", line);
    write(fd, line, strlen(line));
    write(fd, "\n", 1);
}

static void sendf(int fd, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send_line(fd, buf);
}

static int read_line(int fd, char *buf, size_t buf_size)
{
    size_t len = 0;

    while (len + 1 < buf_size) {
        char ch;
        ssize_t ret = read(fd, &ch, 1);

        if (ret == 0) {
            return 0;
        }
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        buf[len++] = ch;
    }

    buf[len] = '\0';
    if (len > 0) {
        printf("bridge rx: %s\n", buf);
    }
    return (int)len;
}

static int get_brightness_value(int *value)
{
    unsigned char v = 0;
    int ret = IMP_ISP_Tuning_GetBrightness(&v);
    *value = (int)v;
    return ret;
}

static int set_brightness_value(int value)
{
    return IMP_ISP_Tuning_SetBrightness((unsigned char)value);
}

static int get_contrast_value(int *value)
{
    unsigned char v = 0;
    int ret = IMP_ISP_Tuning_GetContrast(&v);
    *value = (int)v;
    return ret;
}

static int set_contrast_value(int value)
{
    return IMP_ISP_Tuning_SetContrast((unsigned char)value);
}

static int get_sharpness_value(int *value)
{
    unsigned char v = 0;
    int ret = IMP_ISP_Tuning_GetSharpness(&v);
    *value = (int)v;
    return ret;
}

static int set_sharpness_value(int value)
{
    return IMP_ISP_Tuning_SetSharpness((unsigned char)value);
}

static int get_saturation_value(int *value)
{
    unsigned char v = 0;
    int ret = IMP_ISP_Tuning_GetSaturation(&v);
    *value = (int)v;
    return ret;
}

static int set_saturation_value(int value)
{
    return IMP_ISP_Tuning_SetSaturation((unsigned char)value);
}

static int get_ae_comp_value(int *value)
{
    return IMP_ISP_Tuning_GetAeComp(value);
}

static int set_ae_comp_value(int value)
{
    return IMP_ISP_Tuning_SetAeComp(value);
}

static int get_dpc_value(int *value)
{
    unsigned int v = 0;
    int ret = IMP_ISP_Tuning_GetDPC_Strength(&v);
    *value = (int)v;
    return ret;
}

static int set_dpc_value(int value)
{
    return IMP_ISP_Tuning_SetDPC_Strength((unsigned int)value);
}

static int get_drc_value(int *value)
{
    unsigned int v = 0;
    int ret = IMP_ISP_Tuning_GetDRC_Strength(&v);
    *value = (int)v;
    return ret;
}

static int set_drc_value(int value)
{
    return IMP_ISP_Tuning_SetDRC_Strength((unsigned int)value);
}

static int get_awb_ct_value(int *value)
{
    unsigned int ct = 0;
    int ret = IMP_ISP_Tuning_GetAWBCt(&ct);
    *value = (int)ct;
    return ret;
}

static int set_awb_ct_value(int value)
{
    unsigned int ct = (unsigned int)value;
    return IMP_ISP_Tuning_SetAwbCt(&ct);
}

static bridge_param_desc_t g_params[] = {
    { "BRIGHTNESS", T23_C3_PARAM_BRIGHTNESS, get_brightness_value, set_brightness_value, 0, 255 },
    { "CONTRAST", T23_C3_PARAM_CONTRAST, get_contrast_value, set_contrast_value, 0, 255 },
    { "SHARPNESS", T23_C3_PARAM_SHARPNESS, get_sharpness_value, set_sharpness_value, 0, 255 },
    { "SATURATION", T23_C3_PARAM_SATURATION, get_saturation_value, set_saturation_value, 0, 255 },
    { "AE_COMP", T23_C3_PARAM_AE_COMP, get_ae_comp_value, set_ae_comp_value, 90, 250 },
    { "DPC", T23_C3_PARAM_DPC, get_dpc_value, set_dpc_value, 0, 255 },
    { "DRC", T23_C3_PARAM_DRC, get_drc_value, set_drc_value, 0, 255 },
    { "AWB_CT", T23_C3_PARAM_AWB_CT, get_awb_ct_value, set_awb_ct_value, 1500, 12000 },
};

static bridge_param_desc_t *find_param(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); i++) {
        if (strcmp(g_params[i].name, name) == 0) {
            return &g_params[i];
        }
    }

    return NULL;
}

static void strtoupper(char *s)
{
    while (*s != '\0') {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
        s++;
    }
}

static void send_all_values(int fd)
{
    size_t i;

    for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); i++) {
        int value = 0;
        if (g_params[i].get_value(&value) == 0) {
            sendf(fd, "VAL %s %d", g_params[i].name, value);
        } else {
            sendf(fd, "ERR GET %s", g_params[i].name);
        }
    }
    send_line(fd, "OK GET ALL");
}

static void send_calibration(int fd)
{
    sendf(fd,
          "CAL SIZE %u %u",
          (unsigned int)g_calibration.image_width,
          (unsigned int)g_calibration.image_height);
    sendf(fd,
          "CAL POINTS %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
          g_calibration.points[0].x,
          g_calibration.points[0].y,
          g_calibration.points[1].x,
          g_calibration.points[1].y,
          g_calibration.points[2].x,
          g_calibration.points[2].y,
          g_calibration.points[3].x,
          g_calibration.points[3].y,
          g_calibration.points[4].x,
          g_calibration.points[4].y,
          g_calibration.points[5].x,
          g_calibration.points[5].y,
          g_calibration.points[6].x,
          g_calibration.points[6].y,
          g_calibration.points[7].x,
          g_calibration.points[7].y);
    send_line(fd, "OK CAL GET");
}

static void handle_cal_set(int fd, char *args)
{
    t23_border_calibration_t cal;
    int values[18];
    int parsed;
    int i;

    memset(&cal, 0, sizeof(cal));
    parsed = sscanf(args,
                    "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                    &values[0], &values[1], &values[2], &values[3], &values[4], &values[5],
                    &values[6], &values[7], &values[8], &values[9], &values[10], &values[11],
                    &values[12], &values[13], &values[14], &values[15], &values[16], &values[17]);
    if (parsed != 18) {
        send_line(fd, "ERR CAL_SET_FORMAT");
        return;
    }

    cal.image_width = (uint16_t)values[0];
    cal.image_height = (uint16_t)values[1];
    for (i = 0; i < (int)T23_BORDER_POINT_COUNT; ++i) {
        cal.points[i].x = (int16_t)values[2 + i * 2];
        cal.points[i].y = (int16_t)values[3 + i * 2];
    }

    sanitize_calibration(&cal);
    g_calibration = cal;

    send_calibration(fd);
    send_line(fd, "OK CAL SET");
}

static int capture_jpeg_once(unsigned char *out_buf, size_t *out_len)
{
    int ret;
    unsigned int i;
    int chn_id = get_active_jpeg_channel();
    size_t total = 0;
    IMPEncoderCHNStat stat;

    if (chn_id < 0) {
        return -1;
    }

    ret = IMP_Encoder_PollingStream(chn_id, 1000);
    if (ret < 0) {
        return -1;
    }

    /*
     * The preview loop intentionally drops stale encoded frames and keeps only
     * the newest one. Without this, the encoder queue can accumulate older
     * JPEGs and the browser appears to react to ISP changes only after a long
     * delay, even though the parameter write itself already succeeded.
     */
    while (IMP_Encoder_Query(chn_id, &stat) == 0 && stat.leftStreamFrames > 1) {
        IMPEncoderStream stale_stream;

        ret = IMP_Encoder_GetStream(chn_id, &stale_stream, 0);
        if (ret < 0) {
            break;
        }
        IMP_Encoder_ReleaseStream(chn_id, &stale_stream);
    }

    {
        IMPEncoderStream stream;

        ret = IMP_Encoder_GetStream(chn_id, &stream, 1);
        if (ret < 0) {
            return -1;
        }

        for (i = 0; i < stream.packCount; i++) {
            if (total + stream.pack[i].length > JPEG_MAX_SIZE) {
                IMP_Encoder_ReleaseStream(chn_id, &stream);
                return -1;
            }
            memcpy(out_buf + total, (void *)stream.pack[i].virAddr, stream.pack[i].length);
            total += stream.pack[i].length;
        }

        IMP_Encoder_ReleaseStream(chn_id, &stream);
    }

    *out_len = total;
    return 0;
}

static int ensure_gpio_exported(int gpio)
{
    char path[64];
    int fd;
    int ret;
    char buf[16];

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);
    if (access(path, F_OK) == 0) {
        return 0;
    }

    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) {
        perror("open gpio export");
        return -1;
    }

    ret = snprintf(buf, sizeof(buf), "%d", gpio);
    if (write(fd, buf, ret) != ret) {
        perror("write gpio export");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int write_text_file(const char *path, const char *value)
{
    int fd;
    ssize_t len;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    len = (ssize_t)strlen(value);
    if (write(fd, value, len) != len) {
        perror("write");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int setup_data_ready_input(void)
{
    char path[128];

    if (ensure_gpio_exported(DATA_READY_GPIO) < 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", DATA_READY_GPIO);
    if (write_text_file(path, "in") < 0) {
        return -1;
    }

    return 0;
}

static int read_data_ready_value(int *value)
{
    char ch;

    if (setup_data_ready_input() < 0) {
        return -1;
    }

    if (g_data_ready_fd < 0) {
        char path[128];

        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", DATA_READY_GPIO);
        g_data_ready_fd = open(path, O_RDONLY);
        if (g_data_ready_fd < 0) {
            perror(path);
            return -1;
        }
    }

    if (lseek(g_data_ready_fd, 0, SEEK_SET) < 0) {
        perror("lseek gpio value");
        return -1;
    }
    if (read(g_data_ready_fd, &ch, 1) != 1) {
        perror("read gpio value");
        return -1;
    }
    if (ch == '0') {
        *value = 0;
    } else if (ch == '1') {
        *value = 1;
    } else {
        fprintf(stderr, "unexpected gpio value: %c\n", ch);
        return -1;
    }

    return 0;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int wait_data_ready_high(int timeout_ms)
{
    long long start;
    int value;

    start = monotonic_ms();
    while ((monotonic_ms() - start) < timeout_ms) {
        if (read_data_ready_value(&value) < 0) {
            return -1;
        }
        if (value == 1) {
            return 0;
        }
        usleep(100);
    }

    return 1;
}

static int spi_open_configure(int *fd_out)
{
    uint8_t mode = SPI_MODE_DEFAULT;
    uint8_t bits = SPI_BITS_DEFAULT;
    uint32_t speed = SPI_SPEED_DEFAULT;

    if (g_spi_fd < 0) {
        g_spi_fd = open(SPI_DEVICE, O_RDWR);
        if (g_spi_fd < 0) {
            perror(SPI_DEVICE);
            return -1;
        }
    }

    if (ioctl(g_spi_fd, SPI_IOC_WR_MODE, &mode) < 0) {
        perror("SPI_IOC_WR_MODE");
        return -1;
    }
    if (ioctl(g_spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        perror("SPI_IOC_WR_BITS_PER_WORD");
        return -1;
    }
    if (ioctl(g_spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("SPI_IOC_WR_MAX_SPEED_HZ");
        return -1;
    }

    *fd_out = g_spi_fd;
    return 0;
}

static int spi_transfer_frame(const t23_c3_frame_t *tx_frame, t23_c3_frame_t *rx_frame)
{
    int fd;
    int ret;
    struct spi_ioc_transfer tr;

    if (spi_open_configure(&fd) < 0) {
        return -1;
    }

    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx_frame;
    tr.rx_buf = (unsigned long)rx_frame;
    tr.len = T23_C3_SPI_FRAME_SIZE;
    tr.speed_hz = SPI_SPEED_DEFAULT;
    tr.bits_per_word = SPI_BITS_DEFAULT;

    ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < 0) {
        perror("SPI_IOC_MESSAGE");
        return -1;
    }

    return 0;
}

static int push_one_spi_frame(const t23_c3_frame_t *frame)
{
    t23_c3_frame_t rx_frame;
    int ret;

    ret = wait_data_ready_high(3000);
    if (ret != 0) {
        return -1;
    }

    memset(&rx_frame, 0, sizeof(rx_frame));
    return spi_transfer_frame(frame, &rx_frame);
}

static void fill_frame_header(t23_c3_frame_t *frame,
                              uint8_t type,
                              uint16_t seq,
                              uint8_t param_id,
                              uint8_t status,
                              uint16_t payload_len,
                              uint32_t total_len,
                              uint32_t offset)
{
    memset(frame, 0, sizeof(*frame));
    frame->hdr.magic0 = T23_C3_FRAME_MAGIC0;
    frame->hdr.magic1 = T23_C3_FRAME_MAGIC1;
    frame->hdr.version = (uint8_t)T23_C3_PROTO_VERSION;
    frame->hdr.type = type;
    frame->hdr.param_id = param_id;
    frame->hdr.status = status;
    frame->hdr.seq = seq;
    frame->hdr.payload_len = payload_len;
    frame->hdr.total_len = total_len;
    frame->hdr.offset = offset;
}

static int push_jpeg_over_spi(const unsigned char *jpeg_buf, size_t jpeg_len)
{
    t23_c3_frame_t frame;
    size_t offset = 0;
    uint16_t seq = 1;

    fill_frame_header(&frame,
                      T23_C3_FRAME_TYPE_RESP_JPEG_INFO,
                      seq++,
                      T23_C3_PARAM_NONE,
                      T23_C3_STATUS_OK,
                      sizeof(t23_c3_jpeg_info_payload_t),
                      (uint32_t)jpeg_len,
                      0);
    {
        t23_c3_jpeg_info_payload_t info;
        info.jpeg_len = (uint32_t)jpeg_len;
        memcpy(frame.payload, &info, sizeof(info));
    }
    if (push_one_spi_frame(&frame) < 0) {
        return -1;
    }

    while (offset < jpeg_len) {
        size_t chunk_len = jpeg_len - offset;

        if (chunk_len > T23_C3_FRAME_PAYLOAD_MAX) {
            chunk_len = T23_C3_FRAME_PAYLOAD_MAX;
        }

        fill_frame_header(&frame,
                          T23_C3_FRAME_TYPE_RESP_JPEG_DATA,
                          seq++,
                          T23_C3_PARAM_NONE,
                          T23_C3_STATUS_OK,
                          (uint16_t)chunk_len,
                          (uint32_t)jpeg_len,
                          (uint32_t)offset);
        memcpy(frame.payload, jpeg_buf + offset, chunk_len);

        if (push_one_spi_frame(&frame) < 0) {
            return -1;
        }

        offset += chunk_len;
    }

    return 0;
}

static void handle_snap(int fd)
{
    size_t jpeg_len = 0;

    if (capture_jpeg_once(g_jpeg_buf, &jpeg_len) < 0) {
        send_line(fd, "ERR SNAP");
        return;
    }

    sendf(fd, "SNAP OK %u", (unsigned int)jpeg_len);
    if (push_jpeg_over_spi(g_jpeg_buf, jpeg_len) < 0) {
        send_line(fd, "ERR SNAP_SPI");
    }
}

static void handle_cal_snap(int fd)
{
    size_t jpeg_len = 0;
    size_t rectified_len = 0;

    if (capture_jpeg_once(g_jpeg_buf, &jpeg_len) < 0) {
        send_line(fd, "ERR CAL_SNAP");
        return;
    }

    if (rectify_jpeg_from_calibration(g_jpeg_buf, jpeg_len, g_jpeg_buf, sizeof(g_jpeg_buf), &rectified_len) < 0) {
        send_line(fd, "ERR CAL_RECTIFY");
        return;
    }

    sendf(fd, "CAL SNAP OK %u", (unsigned int)rectified_len);
    if (push_jpeg_over_spi(g_jpeg_buf, rectified_len) < 0) {
        send_line(fd, "ERR CAL_SNAP_SPI");
    }
}

static void process_command(int fd, char *line)
{
    char *cmd;
    char *arg1;
    char *arg2;
    char *saveptr = NULL;

    cmd = strtok_r(line, " ", &saveptr);
    if (cmd == NULL) {
        return;
    }
    strtoupper(cmd);

    if (strcmp(cmd, "PING") == 0) {
        send_line(fd, "PONG");
        return;
    }

    if (strcmp(cmd, "HELP") == 0) {
        send_line(fd, "INFO commands: PING, HELP, MODE GET|SET <DEBUG|RUN>, LAYOUT GET|SET <16X9|4X3>, GET <PARAM|ALL>, SET <PARAM> <VALUE>, SNAP, FRAME, CAL GET, CAL SET, CAL SNAP, BLOCKS GET");
        return;
    }

    if (strcmp(cmd, "MODE") == 0) {
        arg1 = strtok_r(NULL, " ", &saveptr);
        if (arg1 == NULL) {
            send_line(fd, "ERR missing-mode-subcommand");
            return;
        }
        strtoupper(arg1);

        if (strcmp(arg1, "GET") == 0) {
            send_mode_status(fd);
            return;
        }

        if (strcmp(arg1, "SET") == 0) {
            arg2 = strtok_r(NULL, " ", &saveptr);
            if (arg2 == NULL) {
                send_line(fd, "ERR missing-mode-value");
                return;
            }
            strtoupper(arg2);

            if (strcmp(arg2, "DEBUG") == 0) {
                g_bridge_mode = BRIDGE_MODE_DEBUG;
                send_mode_status(fd);
                send_line(fd, "OK MODE SET DEBUG");
                return;
            }
            if (strcmp(arg2, "RUN") == 0) {
                g_bridge_mode = BRIDGE_MODE_RUN;
                send_mode_status(fd);
                send_line(fd, "OK MODE SET RUN");
                return;
            }

            send_line(fd, "ERR unknown-mode");
            return;
        }

        send_line(fd, "ERR unknown-mode-subcommand");
        return;
    }

    if (strcmp(cmd, "LAYOUT") == 0) {
        arg1 = strtok_r(NULL, " ", &saveptr);
        if (arg1 == NULL) {
            send_line(fd, "ERR missing-layout-subcommand");
            return;
        }
        strtoupper(arg1);

        if (strcmp(arg1, "GET") == 0) {
            send_layout_status(fd);
            return;
        }

        if (strcmp(arg1, "SET") == 0) {
            arg2 = strtok_r(NULL, " ", &saveptr);
            if (arg2 == NULL) {
                send_line(fd, "ERR missing-layout-value");
                return;
            }
            strtoupper(arg2);

            if (strcmp(arg2, "16X9") == 0) {
                g_border_layout = T23_BORDER_LAYOUT_16X9;
                send_layout_status(fd);
                send_line(fd, "OK LAYOUT SET 16X9");
                return;
            }
            if (strcmp(arg2, "4X3") == 0) {
                g_border_layout = T23_BORDER_LAYOUT_4X3;
                send_layout_status(fd);
                send_line(fd, "OK LAYOUT SET 4X3");
                return;
            }

            send_line(fd, "ERR unknown-layout");
            return;
        }

        send_line(fd, "ERR unknown-layout-subcommand");
        return;
    }

    if (strcmp(cmd, "SNAP") == 0) {
        handle_snap(fd);
        return;
    }

    if (strcmp(cmd, "FRAME") == 0) {
        handle_frame_with_blocks(fd);
        return;
    }

    if (strcmp(cmd, "BLOCKS") == 0) {
        arg1 = strtok_r(NULL, " ", &saveptr);
        if (arg1 == NULL) {
            send_line(fd, "ERR missing-blocks-subcommand");
            return;
        }
        strtoupper(arg1);
        if (strcmp(arg1, "GET") == 0) {
            handle_blocks_get(fd);
            return;
        }
        send_line(fd, "ERR unknown-blocks-subcommand");
        return;
    }

    if (strcmp(cmd, "CAL") == 0) {
        arg1 = strtok_r(NULL, " ", &saveptr);
        if (arg1 == NULL) {
            send_line(fd, "ERR missing-cal-subcommand");
            return;
        }
        strtoupper(arg1);

        if (strcmp(arg1, "GET") == 0) {
            send_calibration(fd);
            return;
        }

        if (strcmp(arg1, "SET") == 0) {
            char *rest = saveptr;

            if (rest == NULL) {
                send_line(fd, "ERR missing-cal-data");
                return;
            }
            handle_cal_set(fd, rest);
            return;
        }

        if (strcmp(arg1, "SNAP") == 0) {
            handle_cal_snap(fd);
            return;
        }

        send_line(fd, "ERR unknown-cal-subcommand");
        return;
    }

    arg1 = strtok_r(NULL, " ", &saveptr);
    if (arg1 == NULL) {
        send_line(fd, "ERR missing-arg");
        return;
    }
    strtoupper(arg1);

    if (strcmp(cmd, "GET") == 0) {
        if (strcmp(arg1, "ALL") == 0) {
            send_all_values(fd);
            return;
        }
        {
            bridge_param_desc_t *param = find_param(arg1);
            int value = 0;
            if (param == NULL) {
                send_line(fd, "ERR unknown-parameter");
                return;
            }
            if (param->get_value(&value) < 0) {
                sendf(fd, "ERR GET %s", param->name);
                return;
            }
            sendf(fd, "VAL %s %d", param->name, value);
            return;
        }
    }

    if (strcmp(cmd, "SET") == 0) {
        int value = 0;
        bridge_param_desc_t *param = find_param(arg1);

        arg2 = strtok_r(NULL, " ", &saveptr);
        if (param == NULL) {
            send_line(fd, "ERR unknown-parameter");
            return;
        }
        if (arg2 == NULL) {
            send_line(fd, "ERR missing-value");
            return;
        }
        value = atoi(arg2);
        if (value < param->min_value || value > param->max_value) {
            sendf(fd, "ERR range %s %d %d", param->name, param->min_value, param->max_value);
            return;
        }
        if (param->set_value(value) < 0) {
            sendf(fd, "ERR SET %s", param->name);
            return;
        }
        if (param->get_value(&value) < 0) {
            sendf(fd, "OK SET %s", param->name);
            return;
        }
        sendf(fd, "VAL %s %d", param->name, value);
        sendf(fd, "OK SET %s", param->name);
        return;
    }

    send_line(fd, "ERR unknown-command");
}

int main(int argc, char *argv[])
{
    bridge_cfg_t cfg;
    int serial_fd;
    char line[UART_RX_BUF_SIZE];

    if (parse_args(argc, argv, &cfg) < 0) {
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("t23_isp_bridge start\n");
    printf("serial device: %s\n", cfg.serial_device);
    init_default_calibration();

    if (startup_pipeline() < 0) {
        fprintf(stderr, "startup_pipeline failed\n");
        return 1;
    }

    serial_fd = open_serial_port(cfg.serial_device, cfg.baud_rate);
    if (serial_fd < 0) {
        shutdown_pipeline();
        return 1;
    }

    send_line(serial_fd, "READY T23_ISP_BRIDGE 1");

    while (g_running) {
        int ret = read_line(serial_fd, line, sizeof(line));

        if (ret == 0) {
            usleep(10000);
            continue;
        }
        if (ret < 0) {
            perror("read_line");
            break;
        }
        process_command(serial_fd, line);
    }

    close(serial_fd);
    close_transport_resources();
    shutdown_pipeline();
    return 0;
}
