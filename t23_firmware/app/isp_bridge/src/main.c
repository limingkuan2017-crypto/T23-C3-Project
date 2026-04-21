#include <errno.h>
#include <fcntl.h>
#include <float.h>
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
#include "fisheye_valid_mask_640x320.h"
#include "t23_border_pipeline.h"
#include "t23_c3_protocol.h"

#define UART_DEFAULT_DEVICE "/dev/ttyS0"
#define UART_DEFAULT_BAUD B115200
#define UART_RX_BUF_SIZE 512
#define JPEG_MAX_SIZE (512 * 1024)
#define RECTIFIED_JPEG_QUALITY 75
#define RECTIFIED_MAX_WIDTH 640
#define RECTIFIED_MAX_HEIGHT 360
#define HIRES_SNAPSHOT_WIDTH 1280
#define HIRES_SNAPSHOT_HEIGHT 720
#define FISHEYE_CALIB_WIDTH 640
#define FISHEYE_CALIB_HEIGHT 320
#define FISHEYE_FX 297.22593501
#define FISHEYE_FY 263.50077004
#define FISHEYE_CX 317.25653706
#define FISHEYE_CY 215.36940626
#define FISHEYE_K1 -0.11463113
#define FISHEYE_K2 -0.04991202
#define FISHEYE_K3 0.08392980
#define FISHEYE_K4 -0.03815913
#define FISHEYE_KNEW_SCALE 0.56
#define FISHEYE_UI_KNEW_SCALE 0.50
#define FISHEYE_UI_CX_OFFSET 0.0
#define FISHEYE_UI_CY_OFFSET 14.0
#define RECTIFY_FISHEYE_BLEND 0.42
#define TOP_CORNER_USER_BLEND 0.18

/*
 * Current debug setup:
 * - 27" 16:9 display => about 33.6 cm visible height
 * - camera-to-screen distance about 11 cm
 * - top edge midpoint sits about 5 cm above the visible estimate when the
 *   camera is mounted above the TV
 *
 * Use these as a geometric prior to expand the top edge slightly more than
 * the old fixed 12% heuristic so the rectified preview keeps more headroom.
 */
#define DISPLAY_HEIGHT_CM 33.6
#define CAMERA_DISTANCE_CM 11.0
#define TOP_EDGE_OFFSET_CM 5.0

#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_MODE_DEFAULT SPI_MODE_0
#define SPI_BITS_DEFAULT 8
#define SPI_SPEED_DEFAULT 20000000U
#define DATA_READY_GPIO 53
#define SPI_DATA_READY_TIMEOUT_MS 3000
#define RUN_SPI_DATA_READY_TIMEOUT_MS 60

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

typedef struct {
    double a;
    double b;
    double c;
} line2d_t;

typedef struct {
    pointf_t pts_distorted[T23_BORDER_POINT_COUNT];
    pointf_t pts_undistorted[T23_BORDER_POINT_COUNT];
    pointf_t pts_expanded[T23_BORDER_POINT_COUNT];
    double cx;
    double cy;
    double scale;
    double k1;
    int use_fisheye;
    double fish_fx;
    double fish_fy;
    double fish_cx;
    double fish_cy;
    double fish_k[4];
    double fish_fx_new;
    double fish_fy_new;
    double fish_cx_new;
    double fish_cy_new;
    double homography[9];
    double homography_inv[9];
    int rectified_width;
    int rectified_height;
    int crop_left;
    int crop_top;
    int crop_width;
    int crop_height;
    double crop_valid_ratio;
    double tm_rect_y;
} rectification_model_t;

typedef struct {
    int valid;
    int src_w;
    int src_h;
    t23_border_calibration_t calibration;
    rectification_model_t model;
} runtime_rectification_cache_t;

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
static runtime_rectification_cache_t g_runtime_rectification_cache;
static bridge_mode_t g_bridge_mode = BRIDGE_MODE_DEBUG;
static t23_border_layout_t g_border_layout = T23_BORDER_LAYOUT_16X9;
static int g_temper_value_shadow = 128;
static int g_sinter_value_shadow = 128;

static void send_line(int fd, const char *line);
static void sendf(int fd, const char *fmt, ...);
static int capture_jpeg_once(unsigned char *out_buf, size_t *out_len);
static int push_jpeg_over_spi(const unsigned char *jpeg_buf, size_t jpeg_len);
static int push_preview_mosaic_over_spi(const unsigned char *rgb_buf, size_t rgb_len);
static int push_runtime_preview_mosaic_over_spi(const unsigned char *rgb_buf, size_t rgb_len);
static void process_command(int fd, char *line);
static int get_active_framesource_channel(void);
static int startup_pipeline_with_cfg(const sample_sensor_cfg_t *sensor_cfg);
static int finalize_rectification_crop(rectification_model_t *model, int src_w, int src_h);
static void sanitize_calibration(t23_border_calibration_t *cal);

static void invalidate_runtime_rectification_cache(void)
{
    memset(&g_runtime_rectification_cache, 0, sizeof(g_runtime_rectification_cache));
}

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

static sample_sensor_cfg_t make_sensor_cfg_with_size(int width, int height)
{
    sample_sensor_cfg_t cfg = make_sensor_cfg();

    cfg.sensor_width = width;
    cfg.sensor_height = height;
    cfg.chn0_en = 1;
    cfg.chn1_en = 0;
    cfg.chn2_en = 0;
    cfg.chn3_en = 0;
    return cfg;
}

static void init_default_calibration(void)
{
    int w = SENSOR_WIDTH;
    int h = SENSOR_HEIGHT;

    memset(&g_calibration, 0, sizeof(g_calibration));
    g_calibration.image_width = (uint16_t)w;
    g_calibration.image_height = (uint16_t)h;

    /*
     * Match the Python tuner defaults so "reset" starts from the same
     * trapezoid instead of a flat top-edge rectangle.
     */
    g_calibration.points[T23_BORDER_POINT_TL].x = (int16_t)lrint((double)w * 0.02);
    g_calibration.points[T23_BORDER_POINT_TL].y = (int16_t)lrint((double)h * 0.42);
    g_calibration.points[T23_BORDER_POINT_TM].x = (int16_t)lrint((double)w * 0.50);
    g_calibration.points[T23_BORDER_POINT_TM].y = (int16_t)lrint((double)h * 0.23);
    g_calibration.points[T23_BORDER_POINT_TR].x = (int16_t)lrint((double)w * 0.98);
    g_calibration.points[T23_BORDER_POINT_TR].y = (int16_t)lrint((double)h * 0.42);
    g_calibration.points[T23_BORDER_POINT_RM].x = (int16_t)lrint((double)w * 0.90);
    g_calibration.points[T23_BORDER_POINT_RM].y = (int16_t)lrint((double)h * 0.54);
    g_calibration.points[T23_BORDER_POINT_BR].x = (int16_t)lrint((double)w * 0.80);
    g_calibration.points[T23_BORDER_POINT_BR].y = (int16_t)lrint((double)h * 0.65);
    g_calibration.points[T23_BORDER_POINT_BM].x = (int16_t)lrint((double)w * 0.50);
    g_calibration.points[T23_BORDER_POINT_BM].y = (int16_t)lrint((double)h * 0.64);
    g_calibration.points[T23_BORDER_POINT_BL].x = (int16_t)lrint((double)w * 0.18);
    g_calibration.points[T23_BORDER_POINT_BL].y = (int16_t)lrint((double)h * 0.64);
    g_calibration.points[T23_BORDER_POINT_LM].x = (int16_t)lrint((double)w * 0.08);
    g_calibration.points[T23_BORDER_POINT_LM].y = (int16_t)lrint((double)h * 0.54);
    sanitize_calibration(&g_calibration);
    invalidate_runtime_rectification_cache();
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

static double point_set_line_residual(const pointf_t *pts, unsigned int count)
{
    double mean_x = 0.0;
    double mean_y = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;
    double disc;
    double lambda_min;
    unsigned int i;

    if (count < 2u) {
        return 0.0;
    }

    for (i = 0; i < count; ++i) {
        mean_x += pts[i].x;
        mean_y += pts[i].y;
    }
    mean_x /= (double)count;
    mean_y /= (double)count;

    for (i = 0; i < count; ++i) {
        double dx = pts[i].x - mean_x;
        double dy = pts[i].y - mean_y;

        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }

    disc = (sxx - syy) * (sxx - syy) + 4.0 * sxy * sxy;
    lambda_min = 0.5 * (sxx + syy - sqrt(disc));
    return lambda_min / (double)count;
}

static int fit_line_tls(const pointf_t *pts, unsigned int count, line2d_t *line)
{
    double mean_x = 0.0;
    double mean_y = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;
    double theta;
    double dir_x;
    double dir_y;
    double norm;
    unsigned int i;

    if (count < 2u) {
        return -1;
    }

    for (i = 0; i < count; ++i) {
        mean_x += pts[i].x;
        mean_y += pts[i].y;
    }
    mean_x /= (double)count;
    mean_y /= (double)count;

    for (i = 0; i < count; ++i) {
        double dx = pts[i].x - mean_x;
        double dy = pts[i].y - mean_y;

        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }

    theta = 0.5 * atan2(2.0 * sxy, sxx - syy);
    dir_x = cos(theta);
    dir_y = sin(theta);
    norm = sqrt(dir_x * dir_x + dir_y * dir_y);
    if (norm < 1e-9) {
        return -1;
    }

    dir_x /= norm;
    dir_y /= norm;

    line->a = -dir_y;
    line->b = dir_x;
    line->c = -(line->a * mean_x + line->b * mean_y);
    return 0;
}

static int intersect_lines(const line2d_t *l1, const line2d_t *l2, pointf_t *out)
{
    double det = l1->a * l2->b - l2->a * l1->b;

    if (fabs(det) < 1e-9) {
        return -1;
    }

    out->x = (l1->b * l2->c - l2->b * l1->c) / det;
    out->y = (l2->a * l1->c - l1->a * l2->c) / det;
    return 0;
}

static int undistort_point_with_k1(const pointf_t *distorted,
                                   double cx,
                                   double cy,
                                   double scale,
                                   double k1,
                                   pointf_t *undistorted)
{
    double xd = (distorted->x - cx) / scale;
    double yd = (distorted->y - cy) / scale;
    double xu = xd;
    double yu = yd;
    int iter;

    for (iter = 0; iter < 8; ++iter) {
        double ru2 = xu * xu + yu * yu;
        double factor = 1.0 + k1 * ru2;

        if (factor < 0.15) {
            return -1;
        }

        xu = xd / factor;
        yu = yd / factor;
    }

    undistorted->x = xu * scale + cx;
    undistorted->y = yu * scale + cy;
    return 0;
}

static int distort_point_with_k1(const rectification_model_t *model,
                                 const pointf_t *undistorted,
                                 pointf_t *distorted)
{
    double xu = (undistorted->x - model->cx) / model->scale;
    double yu = (undistorted->y - model->cy) / model->scale;
    double ru2 = xu * xu + yu * yu;
    double factor = 1.0 + model->k1 * ru2;

    if (factor < 0.15) {
        return -1;
    }

    distorted->x = xu * factor * model->scale + model->cx;
    distorted->y = yu * factor * model->scale + model->cy;
    return 0;
}

static int init_fisheye_profile(rectification_model_t *model,
                                int src_w,
                                int src_h,
                                double knew_scale,
                                double cx_offset,
                                double cy_offset)
{
    if (src_w != FISHEYE_CALIB_WIDTH || src_h != FISHEYE_CALIB_HEIGHT) {
        return -1;
    }

    model->use_fisheye = 1;
    model->fish_fx = FISHEYE_FX;
    model->fish_fy = FISHEYE_FY;
    model->fish_cx = FISHEYE_CX;
    model->fish_cy = FISHEYE_CY;
    model->fish_k[0] = FISHEYE_K1;
    model->fish_k[1] = FISHEYE_K2;
    model->fish_k[2] = FISHEYE_K3;
    model->fish_k[3] = FISHEYE_K4;

    /*
     * Match the Python calibration tool: build knew by scaling only fx/fy
     * while keeping the calibrated principal point cx/cy unchanged.
     * Optional UI offsets are applied on top only for the calibration canvas.
     */
    model->fish_fx_new = FISHEYE_FX * knew_scale;
    model->fish_fy_new = FISHEYE_FY * knew_scale;
    model->fish_cx_new = FISHEYE_CX + cx_offset;
    model->fish_cy_new = FISHEYE_CY + cy_offset;
    return 0;
}

static int init_fixed_fisheye_profile(rectification_model_t *model, int src_w, int src_h)
{
    /*
     * Final rectification uses a moderate widened FOV that keeps the TV image
     * complete without introducing too much geometric stretch.
     */
    return init_fisheye_profile(model, src_w, src_h, FISHEYE_KNEW_SCALE, 0.0, 0.0);
}

static int fisheye_point_is_valid_by_opencv_mask(const pointf_t *distorted)
{
    int x = (int)lrint(distorted->x);
    int y = (int)lrint(distorted->y);
    size_t index;
    unsigned char bits;

    if (x < 0 || x >= FISHEYE_VALID_MASK_W || y < 0 || y >= FISHEYE_VALID_MASK_H) {
        return 0;
    }

    index = (size_t)y * (size_t)FISHEYE_VALID_MASK_W + (size_t)x;
    bits = g_fisheye_valid_mask_640x320[index >> 3];
    return (bits >> (index & 7)) & 0x1;
}

static int init_calibration_fisheye_profile(rectification_model_t *model, int src_w, int src_h)
{
    /*
     * The calibration canvas prioritizes visibility of the top screen border
     * and both upper corners over visual neatness. A smaller virtual focal
     * length plus a slight downward shift makes dragging points far more stable
     * than calibrating directly on the tighter final-rectification view.
     */
    return init_fisheye_profile(model,
                                src_w,
                                src_h,
                                FISHEYE_UI_KNEW_SCALE,
                                FISHEYE_UI_CX_OFFSET,
                                FISHEYE_UI_CY_OFFSET);
}

static int undistort_point_with_fisheye(const rectification_model_t *model,
                                        const pointf_t *distorted,
                                        pointf_t *undistorted)
{
    double xd = (distorted->x - model->fish_cx) / model->fish_fx;
    double yd = (distorted->y - model->fish_cy) / model->fish_fy;
    double rd = sqrt(xd * xd + yd * yd);
    double theta_d;
    double theta;
    int iter;

    if (!fisheye_point_is_valid_by_opencv_mask(distorted)) {
        return -1;
    }

    if (rd < 1e-12) {
        undistorted->x = model->fish_cx_new;
        undistorted->y = model->fish_cy_new;
        return 0;
    }

    theta_d = rd;
    theta = theta_d;
    for (iter = 0; iter < 10; ++iter) {
        double t2 = theta * theta;
        double t4 = t2 * t2;
        double t6 = t4 * t2;
        double t8 = t4 * t4;
        double poly = 1.0 +
                      model->fish_k[0] * t2 +
                      model->fish_k[1] * t4 +
                      model->fish_k[2] * t6 +
                      model->fish_k[3] * t8;
        double f = theta * poly - theta_d;
        double dpoly = 1.0 +
                       3.0 * model->fish_k[0] * t2 +
                       5.0 * model->fish_k[1] * t4 +
                       7.0 * model->fish_k[2] * t6 +
                       9.0 * model->fish_k[3] * t8;

        if (fabs(dpoly) < 1e-12) {
            break;
        }
        theta -= f / dpoly;
    }

    if (fabs(theta) > (M_PI * 0.495)) {
        return -1;
    }

    {
        double r = tan(theta);
        double scale = r / rd;
        double xu = xd * scale;
        double yu = yd * scale;

        undistorted->x = xu * model->fish_fx_new + model->fish_cx_new;
        undistorted->y = yu * model->fish_fy_new + model->fish_cy_new;
    }
    if (undistorted->x < -(double)FISHEYE_CALIB_WIDTH * 4.0 ||
        undistorted->x >  (double)FISHEYE_CALIB_WIDTH * 4.0 ||
        undistorted->y < -(double)FISHEYE_CALIB_HEIGHT * 4.0 ||
        undistorted->y >  (double)FISHEYE_CALIB_HEIGHT * 4.0) {
        return -1;
    }
    return 0;
}

static int distort_point_with_fisheye(const rectification_model_t *model,
                                      const pointf_t *undistorted,
                                      pointf_t *distorted)
{
    double xu = (undistorted->x - model->fish_cx_new) / model->fish_fx_new;
    double yu = (undistorted->y - model->fish_cy_new) / model->fish_fy_new;
    double r = sqrt(xu * xu + yu * yu);
    double xd;
    double yd;

    if (r < 1e-12) {
        distorted->x = model->fish_cx;
        distorted->y = model->fish_cy;
        return 0;
    }

    {
        double theta = atan(r);
        double t2 = theta * theta;
        double t4 = t2 * t2;
        double t6 = t4 * t2;
        double t8 = t4 * t4;
        double theta_d = theta * (1.0 +
                                  model->fish_k[0] * t2 +
                                  model->fish_k[1] * t4 +
                                  model->fish_k[2] * t6 +
                                  model->fish_k[3] * t8);
        double scale = theta_d / r;

        xd = xu * scale;
        yd = yu * scale;
    }

    distorted->x = xd * model->fish_fx + model->fish_cx;
    distorted->y = yd * model->fish_fy + model->fish_cy;
    return 0;
}

static int undistort_point(const rectification_model_t *model,
                           const pointf_t *distorted,
                           pointf_t *undistorted)
{
    if (model->use_fisheye) {
        return undistort_point_with_fisheye(model, distorted, undistorted);
    }
    return undistort_point_with_k1(distorted,
                                   model->cx,
                                   model->cy,
                                   model->scale,
                                   model->k1,
                                   undistorted);
}

static int distort_point(const rectification_model_t *model,
                         const pointf_t *undistorted,
                         pointf_t *distorted)
{
    if (model->use_fisheye) {
        return distort_point_with_fisheye(model, undistorted, distorted);
    }
    return distort_point_with_k1(model, undistorted, distorted);
}

static int invert_3x3_matrix(const double m[9], double inv_out[9])
{
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                 m[1] * (m[3] * m[8] - m[5] * m[6]) +
                 m[2] * (m[3] * m[7] - m[4] * m[6]);

    if (fabs(det) < 1e-9) {
        return -1;
    }

    inv_out[0] = (m[4] * m[8] - m[5] * m[7]) / det;
    inv_out[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    inv_out[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    inv_out[3] = (m[5] * m[6] - m[3] * m[8]) / det;
    inv_out[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    inv_out[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    inv_out[6] = (m[3] * m[7] - m[4] * m[6]) / det;
    inv_out[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    inv_out[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    return 0;
}

static int solve_homography_from_quads(const pointf_t src[4], const pointf_t dst[4], double h_out[9])
{
    double a[8][9];
    int row;
    int col;
    int pivot;

    memset(a, 0, sizeof(a));
    for (row = 0; row < 4; ++row) {
        double x = src[row].x;
        double y = src[row].y;
        double u = dst[row].x;
        double v = dst[row].y;

        a[row * 2][0] = x;
        a[row * 2][1] = y;
        a[row * 2][2] = 1.0;
        a[row * 2][6] = -u * x;
        a[row * 2][7] = -u * y;
        a[row * 2][8] = u;

        a[row * 2 + 1][3] = x;
        a[row * 2 + 1][4] = y;
        a[row * 2 + 1][5] = 1.0;
        a[row * 2 + 1][6] = -v * x;
        a[row * 2 + 1][7] = -v * y;
        a[row * 2 + 1][8] = v;
    }

    for (col = 0; col < 8; ++col) {
        int best_row = col;
        double best_abs = fabs(a[col][col]);

        for (pivot = col + 1; pivot < 8; ++pivot) {
            double cur_abs = fabs(a[pivot][col]);

            if (cur_abs > best_abs) {
                best_abs = cur_abs;
                best_row = pivot;
            }
        }

        if (best_abs < 1e-9) {
            return -1;
        }

        if (best_row != col) {
            double tmp_row[9];

            memcpy(tmp_row, a[col], sizeof(tmp_row));
            memcpy(a[col], a[best_row], sizeof(tmp_row));
            memcpy(a[best_row], tmp_row, sizeof(tmp_row));
        }

        {
            double diag = a[col][col];

            for (pivot = col; pivot < 9; ++pivot) {
                a[col][pivot] /= diag;
            }
        }

        for (row = 0; row < 8; ++row) {
            double factor;

            if (row == col) {
                continue;
            }
            factor = a[row][col];
            if (fabs(factor) < 1e-12) {
                continue;
            }

            for (pivot = col; pivot < 9; ++pivot) {
                a[row][pivot] -= factor * a[col][pivot];
            }
        }
    }

    h_out[0] = a[0][8];
    h_out[1] = a[1][8];
    h_out[2] = a[2][8];
    h_out[3] = a[3][8];
    h_out[4] = a[4][8];
    h_out[5] = a[5][8];
    h_out[6] = a[6][8];
    h_out[7] = a[7][8];
    h_out[8] = 1.0;
    return 0;
}

static int apply_homography(const double h[9], double x, double y, pointf_t *out)
{
    double denom = h[6] * x + h[7] * y + h[8];

    if (fabs(denom) < 1e-9) {
        return -1;
    }

    out->x = (h[0] * x + h[1] * y + h[2]) / denom;
    out->y = (h[3] * x + h[4] * y + h[5]) / denom;
    return 0;
}

static void multiply_3x3_matrix(const double a[9], const double b[9], double out[9])
{
    double r[9];

    r[0] = a[0] * b[0] + a[1] * b[3] + a[2] * b[6];
    r[1] = a[0] * b[1] + a[1] * b[4] + a[2] * b[7];
    r[2] = a[0] * b[2] + a[1] * b[5] + a[2] * b[8];

    r[3] = a[3] * b[0] + a[4] * b[3] + a[5] * b[6];
    r[4] = a[3] * b[1] + a[4] * b[4] + a[5] * b[7];
    r[5] = a[3] * b[2] + a[4] * b[5] + a[5] * b[8];

    r[6] = a[6] * b[0] + a[7] * b[3] + a[8] * b[6];
    r[7] = a[6] * b[1] + a[7] * b[4] + a[8] * b[7];
    r[8] = a[6] * b[2] + a[7] * b[5] + a[8] * b[8];

    memcpy(out, r, sizeof(r));
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

static void compute_rectified_size_from_quad(const pointf_t quad[4], int *out_w, int *out_h)
{
    double top_len = pointf_distance(quad[0], quad[1]);
    double right_len = pointf_distance(quad[1], quad[2]);
    double bottom_len = pointf_distance(quad[2], quad[3]);
    double left_len = pointf_distance(quad[3], quad[0]);
    double width_est = (top_len > bottom_len) ? top_len : bottom_len;
    double height_est = (left_len > right_len) ? left_len : right_len;
    double scale = 1.0;
    int width;
    int height;

    if (width_est < 1.0) {
        width_est = 320.0;
    }
    if (height_est < 1.0) {
        height_est = 180.0;
    }

    if (width_est < 320.0 || height_est < 180.0) {
        double min_scale_w = 320.0 / width_est;
        double min_scale_h = 180.0 / height_est;
        scale = (min_scale_w > min_scale_h) ? min_scale_w : min_scale_h;
    }
    if (width_est * scale > (double)RECTIFIED_MAX_WIDTH ||
        height_est * scale > (double)RECTIFIED_MAX_HEIGHT) {
        double max_scale_w = (double)RECTIFIED_MAX_WIDTH / width_est;
        double max_scale_h = (double)RECTIFIED_MAX_HEIGHT / height_est;
        scale = (max_scale_w < max_scale_h) ? max_scale_w : max_scale_h;
    }

    width = (int)lrint(width_est * scale);
    height = (int)lrint(height_est * scale);

    width = clamp_int(width, 320, RECTIFIED_MAX_WIDTH);
    height = clamp_int(height, 180, RECTIFIED_MAX_HEIGHT);

    *out_w = width;
    *out_h = height;
}

static int finalize_rectification_model_with_k1(rectification_model_t *model)
{
    static const unsigned int bottom_idx[] = {
        T23_BORDER_POINT_BR,
        T23_BORDER_POINT_BM,
        T23_BORDER_POINT_BL
    };
    int point_valid[T23_BORDER_POINT_COUNT];
    pointf_t edge_pts[3];
    pointf_t left_pts[2];
    pointf_t right_pts[2];
    line2d_t top_line;
    line2d_t right_line;
    line2d_t bottom_line;
    line2d_t left_line;
    pointf_t fitted_quad[4];
    pointf_t expanded_quad[4];
    pointf_t dst_quad[4];
    pointf_t tl_pred;
    pointf_t tr_pred;
    pointf_t tl_ref;
    pointf_t tr_ref;
    unsigned int i;

    memset(point_valid, 0, sizeof(point_valid));
    for (i = 0; i < T23_BORDER_POINT_COUNT; ++i) {
        if (undistort_point(model,
                            &model->pts_distorted[i],
                            &model->pts_undistorted[i]) < 0) {
            /*
             * Python keeps the fixed-fisheye path alive even when TL/TR sit so
             * close to the fisheye edge that OpenCV marks them invalid.
             * Those two points are only weak references for the top line, so do
             * not let them knock the whole model back to the conservative
             * fallback path.
             */
            if (i == T23_BORDER_POINT_TL || i == T23_BORDER_POINT_TR) {
                model->pts_undistorted[i].x = -1000000.0;
                model->pts_undistorted[i].y = -1000000.0;
                continue;
            }
            return -1;
        }
        point_valid[i] = 1;
    }

    for (i = 0; i < 3u; ++i) {
        edge_pts[i] = model->pts_undistorted[bottom_idx[i]];
    }
    if (fit_line_tls(edge_pts, 3u, &bottom_line) < 0) {
        return -1;
    }

    left_pts[0] = model->pts_undistorted[T23_BORDER_POINT_BL];
    left_pts[1] = model->pts_undistorted[T23_BORDER_POINT_LM];
    right_pts[0] = model->pts_undistorted[T23_BORDER_POINT_BR];
    right_pts[1] = model->pts_undistorted[T23_BORDER_POINT_RM];

    if (fit_line_tls(left_pts, 2u, &left_line) < 0 ||
        fit_line_tls(right_pts, 2u, &right_line) < 0) {
        return -1;
    }

    /*
     * TL/TR are the noisiest points because they sit near the fisheye edge.
     * Use BL/LM and BR/RM as the primary side constraints, extrapolate a
     * predicted top pair, then only blend a small amount of the dragged TL/TR
     * back in as a weak reference.
     */
    tl_pred.x = 2.0 * model->pts_undistorted[T23_BORDER_POINT_LM].x -
                model->pts_undistorted[T23_BORDER_POINT_BL].x;
    tl_pred.y = 2.0 * model->pts_undistorted[T23_BORDER_POINT_LM].y -
                model->pts_undistorted[T23_BORDER_POINT_BL].y;
    tr_pred.x = 2.0 * model->pts_undistorted[T23_BORDER_POINT_RM].x -
                model->pts_undistorted[T23_BORDER_POINT_BR].x;
    tr_pred.y = 2.0 * model->pts_undistorted[T23_BORDER_POINT_RM].y -
                model->pts_undistorted[T23_BORDER_POINT_BR].y;

    /*
     * Match Python literally here: TL/TR are always mixed into the top-line
     * reference, even when OpenCV marked them invalid near the fisheye edge.
     * That pushes the fitted top edge much farther away, which is exactly the
     * current Python behavior the web preview is being compared against.
     */
    tl_ref.x = tl_pred.x * (1.0 - TOP_CORNER_USER_BLEND) +
               model->pts_undistorted[T23_BORDER_POINT_TL].x * TOP_CORNER_USER_BLEND;
    tl_ref.y = tl_pred.y * (1.0 - TOP_CORNER_USER_BLEND) +
               model->pts_undistorted[T23_BORDER_POINT_TL].y * TOP_CORNER_USER_BLEND;
    tr_ref.x = tr_pred.x * (1.0 - TOP_CORNER_USER_BLEND) +
               model->pts_undistorted[T23_BORDER_POINT_TR].x * TOP_CORNER_USER_BLEND;
    tr_ref.y = tr_pred.y * (1.0 - TOP_CORNER_USER_BLEND) +
               model->pts_undistorted[T23_BORDER_POINT_TR].y * TOP_CORNER_USER_BLEND;

    edge_pts[0] = tl_ref;
    edge_pts[1] = tr_ref;
    if (fit_line_tls(edge_pts, 2u, &top_line) < 0) {
        return -1;
    }

    if (intersect_lines(&top_line, &left_line, &fitted_quad[0]) < 0 ||
        intersect_lines(&top_line, &right_line, &fitted_quad[1]) < 0 ||
        intersect_lines(&bottom_line, &right_line, &fitted_quad[2]) < 0 ||
        intersect_lines(&bottom_line, &left_line, &fitted_quad[3]) < 0) {
        return -1;
    }

    memcpy(model->pts_expanded, model->pts_undistorted, sizeof(model->pts_expanded));

    expanded_quad[0] = fitted_quad[0];
    expanded_quad[1] = fitted_quad[1];
    expanded_quad[2] = fitted_quad[2];
    expanded_quad[3] = fitted_quad[3];

    compute_rectified_size_from_quad(expanded_quad, &model->rectified_width, &model->rectified_height);
    dst_quad[0].x = 0.0;
    dst_quad[0].y = 0.0;
    dst_quad[1].x = (double)(model->rectified_width - 1);
    dst_quad[1].y = 0.0;
    dst_quad[2].x = (double)(model->rectified_width - 1);
    dst_quad[2].y = (double)(model->rectified_height - 1);
    dst_quad[3].x = 0.0;
    dst_quad[3].y = (double)(model->rectified_height - 1);

    if (solve_homography_from_quads(expanded_quad, dst_quad, model->homography) < 0) {
        return -1;
    }
    if (invert_3x3_matrix(model->homography, model->homography_inv) < 0) {
        return -1;
    }

    return 0;
}

static int build_rectification_model(const t23_border_calibration_t *cal,
                                     int src_w,
                                     int src_h,
                                     rectification_model_t *model)
{
    pointf_t tm_rect;

    memset(model, 0, sizeof(*model));
    scaled_calibration_points(cal, src_w, src_h, model->pts_distorted);
    model->cx = (double)(src_w - 1) * 0.5;
    model->cy = (double)(src_h - 1) * 0.5;
    model->scale = ((src_w > src_h) ? (double)src_w : (double)src_h) * 0.5;
    if (model->scale < 1.0) {
        model->scale = 1.0;
    }
    /* Use the fixed offline fisheye calibration as the only primary path. */
    if (init_fixed_fisheye_profile(model, src_w, src_h) == 0 &&
        finalize_rectification_model_with_k1(model) == 0) {
        if (apply_homography(model->homography,
                             model->pts_undistorted[T23_BORDER_POINT_TM].x,
                             model->pts_undistorted[T23_BORDER_POINT_TM].y,
                             &tm_rect) == 0) {
            model->tm_rect_y = tm_rect.y;
        } else {
            model->tm_rect_y = 0.0;
        }
        if (finalize_rectification_crop(model, src_w, src_h) == 0) {
            return 0;
        }
    }

    /*
     * Keep one conservative fallback only: disable lens undistortion and use
     * the 8-point perspective model directly. Do not re-introduce any dynamic
     * k1 fitting here, otherwise TM and other helper points start influencing
     * the lens model again.
     */
    memset(model, 0, sizeof(*model));
    scaled_calibration_points(cal, src_w, src_h, model->pts_distorted);
    model->cx = (double)(src_w - 1) * 0.5;
    model->cy = (double)(src_h - 1) * 0.5;
    model->scale = ((src_w > src_h) ? (double)src_w : (double)src_h) * 0.5;
    if (model->scale < 1.0) {
        model->scale = 1.0;
    }
    model->k1 = 0.0;
    if (finalize_rectification_model_with_k1(model) == 0) {
        if (apply_homography(model->homography,
                             model->pts_undistorted[T23_BORDER_POINT_TM].x,
                             model->pts_undistorted[T23_BORDER_POINT_TM].y,
                             &tm_rect) == 0) {
            model->tm_rect_y = tm_rect.y;
        } else {
            model->tm_rect_y = 0.0;
        }
        if (finalize_rectification_crop(model, src_w, src_h) == 0) {
            return 0;
        }
    }

    return -1;
}

static int build_rectification_model_raw(const t23_border_calibration_t *cal,
                                         int src_w,
                                         int src_h,
                                         rectification_model_t *model)
{
    pointf_t tm_rect;

    memset(model, 0, sizeof(*model));
    scaled_calibration_points(cal, src_w, src_h, model->pts_distorted);
    model->cx = (double)(src_w - 1) * 0.5;
    model->cy = (double)(src_h - 1) * 0.5;
    model->scale = ((src_w > src_h) ? (double)src_w : (double)src_h) * 0.5;
    if (model->scale < 1.0) {
        model->scale = 1.0;
    }
    model->k1 = 0.0;
    if (finalize_rectification_model_with_k1(model) < 0) {
        return -1;
    }
    if (apply_homography(model->homography,
                         model->pts_undistorted[T23_BORDER_POINT_TM].x,
                         model->pts_undistorted[T23_BORDER_POINT_TM].y,
                         &tm_rect) == 0) {
        model->tm_rect_y = tm_rect.y;
    } else {
        model->tm_rect_y = 0.0;
    }
    return finalize_rectification_crop(model, src_w, src_h);
}

static int build_rectification_model_fisheye(const t23_border_calibration_t *cal,
                                             int src_w,
                                             int src_h,
                                             rectification_model_t *model)
{
    pointf_t tm_rect;

    memset(model, 0, sizeof(*model));
    scaled_calibration_points(cal, src_w, src_h, model->pts_distorted);
    model->cx = (double)(src_w - 1) * 0.5;
    model->cy = (double)(src_h - 1) * 0.5;
    model->scale = ((src_w > src_h) ? (double)src_w : (double)src_h) * 0.5;
    if (model->scale < 1.0) {
        model->scale = 1.0;
    }
    if (init_fixed_fisheye_profile(model, src_w, src_h) < 0) {
        return -1;
    }
    if (finalize_rectification_model_with_k1(model) < 0) {
        return -1;
    }
    if (apply_homography(model->homography,
                         model->pts_undistorted[T23_BORDER_POINT_TM].x,
                         model->pts_undistorted[T23_BORDER_POINT_TM].y,
                         &tm_rect) == 0) {
        model->tm_rect_y = tm_rect.y;
    } else {
        model->tm_rect_y = 0.0;
    }
    return finalize_rectification_crop(model, src_w, src_h);
}

static int rectified_point_to_source(const rectification_model_t *model,
                                     double x,
                                     double y,
                                     pointf_t *src_out)
{
    pointf_t undistorted;
    if (apply_homography(model->homography_inv, x, y, &undistorted) < 0) {
        return -1;
    }
    return distort_point(model, &undistorted, src_out);
}

static int rectified_uv_to_source(const rectification_model_t *model,
                                  double u,
                                  double v,
                                  pointf_t *src_out)
{
    double x = u * (double)(model->rectified_width - 1);
    double y = v * (double)(model->rectified_height - 1);

    return rectified_point_to_source(model, x, y, src_out);
}

static int rectified_crop_point_to_source(const rectification_model_t *model,
                                          double x,
                                          double y,
                                          pointf_t *src_out)
{
    return rectified_point_to_source(model,
                                     x + (double)model->crop_left,
                                     y + (double)model->crop_top,
                                     src_out);
}

static int build_direct_valid_mask(const rectification_model_t *model,
                                   int src_w,
                                   int src_h,
                                   uint8_t *valid_mask)
{
    int x;
    int y;

    for (y = 0; y < model->rectified_height; ++y) {
        for (x = 0; x < model->rectified_width; ++x) {
            pointf_t src;
            uint8_t ok = 0;

            if (rectified_point_to_source(model, (double)x, (double)y, &src) == 0 &&
                src.x >= 0.0 && src.x < (double)src_w &&
                src.y >= 0.0 && src.y < (double)src_h) {
                ok = 1;
            }
            valid_mask[y * model->rectified_width + x] = ok;
        }
    }

    return 0;
}

static void build_invalid_integral_image(const uint8_t *valid_mask,
                                         int mask_w,
                                         int mask_h,
                                         uint32_t *integral)
{
    int x;
    int y;
    int stride = mask_w + 1;

    memset(integral, 0, (size_t)stride * (size_t)(mask_h + 1) * sizeof(*integral));
    for (y = 0; y < mask_h; ++y) {
        uint32_t row_sum = 0;
        for (x = 0; x < mask_w; ++x) {
            row_sum += (valid_mask[y * mask_w + x] == 0) ? 1u : 0u;
            integral[(y + 1) * stride + (x + 1)] = integral[y * stride + (x + 1)] + row_sum;
        }
    }
}

static uint32_t count_invalid_pixels(const uint32_t *integral,
                                     int stride,
                                     int x,
                                     int y,
                                     int w,
                                     int h)
{
    int x2 = x + w;
    int y2 = y + h;
    return integral[y2 * stride + x2] - integral[y * stride + x2] -
           integral[y2 * stride + x] + integral[y * stride + x];
}

static int rectified_rect_is_valid(const uint32_t *invalid_integral,
                                   int integral_stride,
                                   int left,
                                   int top,
                                   int right,
                                   int bottom)
{
    if (left > right || top > bottom) {
        return 0;
    }
    return count_invalid_pixels(invalid_integral,
                                integral_stride,
                                left,
                                top,
                                right - left + 1,
                                bottom - top + 1) == 0;
}

static double crop_aspect_error(int width, int height, double desired_aspect)
{
    double aspect;

    if (width <= 0 || height <= 0 || desired_aspect <= 0.0) {
        return DBL_MAX;
    }
    aspect = (double)width / (double)height;
    return fabs(log(aspect / desired_aspect));
}

static void consider_crop_candidate(const uint32_t *invalid_integral,
                                    int integral_stride,
                                    int mask_w,
                                    int mask_h,
                                    int left,
                                    int top,
                                    int right,
                                    int bottom,
                                    double desired_aspect,
                                    int current_area,
                                    int *best_found,
                                    int *best_left,
                                    int *best_top,
                                    int *best_right,
                                    int *best_bottom,
                                    int *best_area,
                                    double *best_error)
{
    int width;
    int height;
    int area;
    double error;

    if (left < 0 || top < 0 || right >= mask_w || bottom >= mask_h) {
        return;
    }
    if (!rectified_rect_is_valid(invalid_integral, integral_stride, left, top, right, bottom)) {
        return;
    }

    width = right - left + 1;
    height = bottom - top + 1;
    area = width * height;
    if (area < current_area) {
        return;
    }

    error = crop_aspect_error(width, height, desired_aspect);
    if (!*best_found ||
        area > *best_area ||
        (area == *best_area && error < *best_error)) {
        *best_found = 1;
        *best_left = left;
        *best_top = top;
        *best_right = right;
        *best_bottom = bottom;
        *best_area = area;
        *best_error = error;
    }
}

static void expand_valid_crop_greedy(const uint32_t *invalid_integral,
                                     int integral_stride,
                                     int mask_w,
                                     int mask_h,
                                     double desired_aspect,
                                     int *left_io,
                                     int *top_io,
                                     int *right_io,
                                     int *bottom_io)
{
    int left = *left_io;
    int top = *top_io;
    int right = *right_io;
    int bottom = *bottom_io;

    while (1) {
        int current_area = (right - left + 1) * (bottom - top + 1);
        int best_found = 0;
        int best_left = left;
        int best_top = top;
        int best_right = right;
        int best_bottom = bottom;
        int best_area = current_area;
        double best_error = crop_aspect_error(right - left + 1,
                                              bottom - top + 1,
                                              desired_aspect);

        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left - 1, top, right, bottom,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left, top - 1, right, bottom,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left, top, right + 1, bottom,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left, top, right, bottom + 1,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left - 1, top, right + 1, bottom,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left, top - 1, right, bottom + 1,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left - 1, top - 1, right, bottom,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left, top - 1, right + 1, bottom,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left - 1, top, right, bottom + 1,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left, top, right + 1, bottom + 1,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);
        consider_crop_candidate(invalid_integral, integral_stride, mask_w, mask_h,
                                left - 1, top - 1, right + 1, bottom + 1,
                                desired_aspect, current_area,
                                &best_found, &best_left, &best_top, &best_right, &best_bottom,
                                &best_area, &best_error);

        if (!best_found) {
            break;
        }

        left = best_left;
        top = best_top;
        right = best_right;
        bottom = best_bottom;
    }

    *left_io = left;
    *top_io = top;
    *right_io = right;
    *bottom_io = bottom;
}

static int find_nearest_valid_seed(const uint8_t *valid_mask,
                                   int mask_w,
                                   int mask_h,
                                   int center_x,
                                   int center_y,
                                   int *x_out,
                                   int *y_out)
{
    int radius;

    center_x = clamp_int(center_x, 0, mask_w - 1);
    center_y = clamp_int(center_y, 0, mask_h - 1);
    if (valid_mask[center_y * mask_w + center_x]) {
        *x_out = center_x;
        *y_out = center_y;
        return 0;
    }

    for (radius = 1; radius < (mask_w > mask_h ? mask_w : mask_h); ++radius) {
        int x0 = clamp_int(center_x - radius, 0, mask_w - 1);
        int x1 = clamp_int(center_x + radius, 0, mask_w - 1);
        int y0 = clamp_int(center_y - radius, 0, mask_h - 1);
        int y1 = clamp_int(center_y + radius, 0, mask_h - 1);
        int x;
        int y;

        for (x = x0; x <= x1; ++x) {
            if (valid_mask[y0 * mask_w + x]) {
                *x_out = x;
                *y_out = y0;
                return 0;
            }
            if (valid_mask[y1 * mask_w + x]) {
                *x_out = x;
                *y_out = y1;
                return 0;
            }
        }
        for (y = y0 + 1; y < y1; ++y) {
            if (valid_mask[y * mask_w + x0]) {
                *x_out = x0;
                *y_out = y;
                return 0;
            }
            if (valid_mask[y * mask_w + x1]) {
                *x_out = x1;
                *y_out = y;
                return 0;
            }
        }
    }

    return -1;
}

static int rectified_point_usable(const pointf_t *pt)
{
    return isfinite(pt->x) && isfinite(pt->y) &&
           fabs(pt->x) < 100000.0 && fabs(pt->y) < 100000.0;
}

static void accumulate_projected_content_point(const rectification_model_t *model,
                                               const pointf_t *src_pt,
                                               double *min_x,
                                               double *min_y,
                                               double *max_x,
                                               double *max_y,
                                               int *have_point)
{
    pointf_t rect_pt;

    if (!rectified_point_usable(src_pt)) {
        return;
    }
    if (apply_homography(model->homography, src_pt->x, src_pt->y, &rect_pt) < 0) {
        return;
    }
    if (!isfinite(rect_pt.x) || !isfinite(rect_pt.y)) {
        return;
    }

    if (!*have_point) {
        *min_x = *max_x = rect_pt.x;
        *min_y = *max_y = rect_pt.y;
        *have_point = 1;
        return;
    }

    if (rect_pt.x < *min_x) {
        *min_x = rect_pt.x;
    }
    if (rect_pt.y < *min_y) {
        *min_y = rect_pt.y;
    }
    if (rect_pt.x > *max_x) {
        *max_x = rect_pt.x;
    }
    if (rect_pt.y > *max_y) {
        *max_y = rect_pt.y;
    }
}

static int compute_required_content_bounds(const rectification_model_t *model,
                                           int *left_out,
                                           int *top_out,
                                           int *right_out,
                                           int *bottom_out)
{
    static const unsigned int content_idx[] = {
        T23_BORDER_POINT_TM,
        T23_BORDER_POINT_LM,
        T23_BORDER_POINT_RM,
        T23_BORDER_POINT_BL,
        T23_BORDER_POINT_BM,
        T23_BORDER_POINT_BR
    };
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
    int have_point = 0;
    unsigned int i;

    for (i = 0; i < sizeof(content_idx) / sizeof(content_idx[0]); ++i) {
        accumulate_projected_content_point(model,
                                           &model->pts_undistorted[content_idx[i]],
                                           &min_x,
                                           &min_y,
                                           &max_x,
                                           &max_y,
                                           &have_point);
    }

    if (rectified_point_usable(&model->pts_undistorted[T23_BORDER_POINT_BL]) &&
        rectified_point_usable(&model->pts_undistorted[T23_BORDER_POINT_LM])) {
        pointf_t tl_pred;
        pointf_t tl_ref;

        tl_pred.x = 2.0 * model->pts_undistorted[T23_BORDER_POINT_LM].x -
                    model->pts_undistorted[T23_BORDER_POINT_BL].x;
        tl_pred.y = 2.0 * model->pts_undistorted[T23_BORDER_POINT_LM].y -
                    model->pts_undistorted[T23_BORDER_POINT_BL].y;
        if (rectified_point_usable(&model->pts_undistorted[T23_BORDER_POINT_TL])) {
            tl_ref.x = tl_pred.x * (1.0 - TOP_CORNER_USER_BLEND) +
                       model->pts_undistorted[T23_BORDER_POINT_TL].x * TOP_CORNER_USER_BLEND;
            tl_ref.y = tl_pred.y * (1.0 - TOP_CORNER_USER_BLEND) +
                       model->pts_undistorted[T23_BORDER_POINT_TL].y * TOP_CORNER_USER_BLEND;
        } else {
            tl_ref = tl_pred;
        }
        accumulate_projected_content_point(model, &tl_ref, &min_x, &min_y, &max_x, &max_y, &have_point);
    }

    if (rectified_point_usable(&model->pts_undistorted[T23_BORDER_POINT_BR]) &&
        rectified_point_usable(&model->pts_undistorted[T23_BORDER_POINT_RM])) {
        pointf_t tr_pred;
        pointf_t tr_ref;

        tr_pred.x = 2.0 * model->pts_undistorted[T23_BORDER_POINT_RM].x -
                    model->pts_undistorted[T23_BORDER_POINT_BR].x;
        tr_pred.y = 2.0 * model->pts_undistorted[T23_BORDER_POINT_RM].y -
                    model->pts_undistorted[T23_BORDER_POINT_BR].y;
        if (rectified_point_usable(&model->pts_undistorted[T23_BORDER_POINT_TR])) {
            tr_ref.x = tr_pred.x * (1.0 - TOP_CORNER_USER_BLEND) +
                       model->pts_undistorted[T23_BORDER_POINT_TR].x * TOP_CORNER_USER_BLEND;
            tr_ref.y = tr_pred.y * (1.0 - TOP_CORNER_USER_BLEND) +
                       model->pts_undistorted[T23_BORDER_POINT_TR].y * TOP_CORNER_USER_BLEND;
        } else {
            tr_ref = tr_pred;
        }
        accumulate_projected_content_point(model, &tr_ref, &min_x, &min_y, &max_x, &max_y, &have_point);
    }

    if (!have_point) {
        return -1;
    }

    *left_out = clamp_int((int)floor(min_x), 0, model->rectified_width - 1);
    *top_out = clamp_int((int)floor(min_y), 0, model->rectified_height - 1);
    *right_out = clamp_int((int)ceil(max_x), *left_out, model->rectified_width - 1);
    *bottom_out = clamp_int((int)ceil(max_y), *top_out, model->rectified_height - 1);
    return 0;
}

static int finalize_rectification_crop(rectification_model_t *model, int src_w, int src_h)
{
    uint8_t *valid_mask = NULL;
    uint32_t *invalid_integral = NULL;
    double desired_cx = (double)model->rectified_width * 0.5;
    double desired_cy = (double)model->rectified_height * 0.5;
    double desired_aspect = (model->rectified_height > 0) ?
                            ((double)model->rectified_width / (double)model->rectified_height) :
                            (16.0 / 9.0);
    int left;
    int top;
    int right;
    int bottom;

    valid_mask = malloc((size_t)model->rectified_width * (size_t)model->rectified_height);
    if (valid_mask == NULL) {
        return -1;
    }
    invalid_integral = malloc((size_t)(model->rectified_width + 1) *
                              (size_t)(model->rectified_height + 1) *
                              sizeof(*invalid_integral));
    if (invalid_integral == NULL) {
        free(valid_mask);
        return -1;
    }

    build_direct_valid_mask(model, src_w, src_h, valid_mask);
    build_invalid_integral_image(valid_mask,
                                 model->rectified_width,
                                 model->rectified_height,
                                 invalid_integral);

    if (compute_required_content_bounds(model, &left, &top, &right, &bottom) < 0 ||
        !rectified_rect_is_valid(invalid_integral,
                                 model->rectified_width + 1,
                                 left,
                                 top,
                                 right,
                                 bottom)) {
        int seed_x = 0;
        int seed_y = 0;

        if (find_nearest_valid_seed(valid_mask,
                                    model->rectified_width,
                                    model->rectified_height,
                                    (int)lrint(desired_cx),
                                    (int)lrint(desired_cy),
                                    &seed_x,
                                    &seed_y) < 0) {
            free(invalid_integral);
            free(valid_mask);
            return -1;
        }
        left = seed_x;
        right = seed_x;
        top = seed_y;
        bottom = seed_y;
    }

    expand_valid_crop_greedy(invalid_integral,
                             model->rectified_width + 1,
                             model->rectified_width,
                             model->rectified_height,
                             desired_aspect,
                             &left,
                             &top,
                             &right,
                             &bottom);

    model->crop_left = left;
    model->crop_top = top;
    model->crop_width = right - left + 1;
    model->crop_height = bottom - top + 1;
    model->crop_valid_ratio = 1.0;

    free(invalid_integral);
    free(valid_mask);
    return 0;
}

static int rectified_uv_to_source_blended(const rectification_model_t *raw_model,
                                          const rectification_model_t *fish_model,
                                          double blend,
                                          double u,
                                          double v,
                                          pointf_t *src_out)
{
    pointf_t raw_src;
    pointf_t fish_src;
    int raw_ok = rectified_uv_to_source(raw_model, u, v, &raw_src) == 0;
    int fish_ok = (fish_model != NULL) && rectified_uv_to_source(fish_model, u, v, &fish_src) == 0;

    if (!raw_ok && !fish_ok) {
        return -1;
    }
    if (raw_ok && !fish_ok) {
        *src_out = raw_src;
        return 0;
    }
    if (!raw_ok && fish_ok) {
        *src_out = fish_src;
        return 0;
    }

    src_out->x = raw_src.x * (1.0 - blend) + fish_src.x * blend;
    src_out->y = raw_src.y * (1.0 - blend) + fish_src.y * blend;
    return 0;
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

static unsigned char bilinear_sample_plane(const unsigned char *plane,
                                           int stride,
                                           int plane_w,
                                           int plane_h,
                                           double x,
                                           double y)
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

    if (x < 0.0 || y < 0.0 || x > (double)(plane_w - 1) || y > (double)(plane_h - 1)) {
        return 0;
    }

    x0 = (int)floor(x);
    y0 = (int)floor(y);
    x1 = (x0 + 1 < plane_w) ? (x0 + 1) : x0;
    y1 = (y0 + 1 < plane_h) ? (y0 + 1) : y0;
    fx = x - x0;
    fy = y - y0;

    idx00 = y0 * stride + x0;
    idx10 = y0 * stride + x1;
    idx01 = y1 * stride + x0;
    idx11 = y1 * stride + x1;

    v00 = plane[idx00];
    v10 = plane[idx10];
    v01 = plane[idx01];
    v11 = plane[idx11];

    top = v00 + (v10 - v00) * fx;
    bottom = v01 + (v11 - v01) * fx;
    value = top + (bottom - top) * fy;

    return (unsigned char)clamp_int((int)lrint(value), 0, 255);
}

static unsigned char bilinear_sample_nv12_uv_channel(const unsigned char *uv_plane,
                                                     int src_w,
                                                     int uv_w,
                                                     int uv_h,
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

    if (x < 0.0 || y < 0.0 || x > (double)(uv_w - 1) || y > (double)(uv_h - 1)) {
        return 128;
    }

    x0 = (int)floor(x);
    y0 = (int)floor(y);
    x1 = (x0 + 1 < uv_w) ? (x0 + 1) : x0;
    y1 = (y0 + 1 < uv_h) ? (y0 + 1) : y0;
    fx = x - x0;
    fy = y - y0;

    idx00 = y0 * src_w + x0 * 2 + channel;
    idx10 = y0 * src_w + x1 * 2 + channel;
    idx01 = y1 * src_w + x0 * 2 + channel;
    idx11 = y1 * src_w + x1 * 2 + channel;

    v00 = uv_plane[idx00];
    v10 = uv_plane[idx10];
    v01 = uv_plane[idx01];
    v11 = uv_plane[idx11];

    top = v00 + (v10 - v00) * fx;
    bottom = v01 + (v11 - v01) * fx;
    value = top + (bottom - top) * fy;

    return (unsigned char)clamp_int((int)lrint(value), 0, 255);
}

static void yuv_to_rgb_bt601(uint8_t y, uint8_t u, uint8_t v, t23_rgb8_t *out)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    int r;
    int g;
    int b;

    if (c < 0) {
        c = 0;
    }

    r = (298 * c + 409 * e + 128) >> 8;
    g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    b = (298 * c + 516 * d + 128) >> 8;

    out->r = (uint8_t)clamp_int(r, 0, 255);
    out->g = (uint8_t)clamp_int(g, 0, 255);
    out->b = (uint8_t)clamp_int(b, 0, 255);
}

static int get_cached_rectification_model(int src_w,
                                          int src_h,
                                          const rectification_model_t **model_out)
{
    if (model_out == NULL) {
        return -1;
    }

    if (g_runtime_rectification_cache.valid &&
        g_runtime_rectification_cache.src_w == src_w &&
        g_runtime_rectification_cache.src_h == src_h &&
        memcmp(&g_runtime_rectification_cache.calibration, &g_calibration, sizeof(g_calibration)) == 0) {
        *model_out = &g_runtime_rectification_cache.model;
        return 0;
    }

    if (build_rectification_model(&g_calibration, src_w, src_h, &g_runtime_rectification_cache.model) < 0) {
        return -1;
    }

    g_runtime_rectification_cache.valid = 1;
    g_runtime_rectification_cache.src_w = src_w;
    g_runtime_rectification_cache.src_h = src_h;
    g_runtime_rectification_cache.calibration = g_calibration;
    *model_out = &g_runtime_rectification_cache.model;
    return 0;
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
    rectification_model_t model;
    int src_w = 0;
    int src_h = 0;
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

    if (build_rectification_model(&g_calibration, src_w, src_h, &model) < 0) {
        free(src_rgb);
        return -1;
    }

    dst_rgb = malloc((size_t)model.crop_width * model.crop_height * 3);
    if (dst_rgb == NULL) {
        free(src_rgb);
        return -1;
    }
    rect_left = 0;
    rect_top = 0;
    rect_right = model.crop_width - 1;
    rect_bottom = model.crop_height - 1;

    for (y = 0; y < model.crop_height; ++y) {
        for (x = 0; x < model.crop_width; ++x) {
            pointf_t src;
            unsigned char *pixel = dst_rgb + (y * model.crop_width + x) * 3;
            if (rectified_crop_point_to_source(&model, (double)x, (double)y, &src) < 0 ||
                src.x < 0.0 || src.x > (double)(src_w - 1) ||
                src.y < 0.0 || src.y > (double)(src_h - 1)) {
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                continue;
            }

            pixel[0] = bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 0);
            pixel[1] = bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 1);
            pixel[2] = bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 2);
        }
    }
    free(src_rgb);
    *dst_rgb_out = dst_rgb;
    *dst_w_out = model.crop_width;
    *dst_h_out = model.crop_height;
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

static void compute_average_rectified_patch(const unsigned char *src_rgb,
                                            int src_w,
                                            int src_h,
                                            const rectification_model_t *model,
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

    left = clamp_int(left, 0, model->crop_width - 1);
    right = clamp_int(right, 0, model->crop_width - 1);
    top = clamp_int(top, 0, model->crop_height - 1);
    bottom = clamp_int(bottom, 0, model->crop_height - 1);
    if (right < left) {
        right = left;
    }
    if (bottom < top) {
        bottom = top;
    }

    for (y = top; y <= bottom; ++y) {
        for (x = left; x <= right; ++x) {
            pointf_t src;

            if (rectified_crop_point_to_source(model, (double)x, (double)y, &src) < 0) {
                continue;
            }

            r_sum += bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 0);
            g_sum += bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 1);
            b_sum += bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 2);
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

static int sample_rectified_rgb(const unsigned char *src_rgb,
                                int src_w,
                                int src_h,
                                const rectification_model_t *model,
                                double x,
                                double y,
                                t23_rgb8_t *out)
{
    pointf_t src;

    if (rectified_crop_point_to_source(model, x, y, &src) < 0 ||
        src.x < 0.0 || src.x > (double)(src_w - 1) ||
        src.y < 0.0 || src.y > (double)(src_h - 1)) {
        return -1;
    }

    out->r = bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 0);
    out->g = bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 1);
    out->b = bilinear_sample_channel(src_rgb, src_w, src_h, src.x, src.y, 2);
    return 0;
}

static int sample_rectified_rgb_nv12(const unsigned char *src_nv12,
                                     int src_w,
                                     int src_h,
                                     const rectification_model_t *model,
                                     double x,
                                     double y,
                                     t23_rgb8_t *out)
{
    const unsigned char *y_plane = src_nv12;
    const unsigned char *uv_plane = src_nv12 + (size_t)src_w * src_h;
    pointf_t src;
    unsigned char y_sample;
    unsigned char u_sample;
    unsigned char v_sample;
    int uv_w;
    int uv_h;

    if (rectified_crop_point_to_source(model, x, y, &src) < 0 ||
        src.x < 0.0 || src.x > (double)(src_w - 1) ||
        src.y < 0.0 || src.y > (double)(src_h - 1)) {
        return -1;
    }

    uv_w = src_w / 2;
    uv_h = src_h / 2;
    if (uv_w <= 0 || uv_h <= 0) {
        return -1;
    }

    y_sample = bilinear_sample_plane(y_plane, src_w, src_w, src_h, src.x, src.y);
    u_sample = bilinear_sample_nv12_uv_channel(uv_plane, src_w, uv_w, uv_h, src.x * 0.5, src.y * 0.5, 0);
    v_sample = bilinear_sample_nv12_uv_channel(uv_plane, src_w, uv_w, uv_h, src.x * 0.5, src.y * 0.5, 1);
    yuv_to_rgb_bt601(y_sample, u_sample, v_sample, out);
    return 0;
}

/*
 * RUN mode favors latency over exact per-cell integration. Sampling a small
 * fixed set of representative points per 16x16 cell keeps the live strip much
 * more responsive than walking every rectified pixel inside the cell.
 */
static void compute_sparse_rectified_patch(const unsigned char *src_rgb,
                                           int src_w,
                                           int src_h,
                                           const rectification_model_t *model,
                                           int left,
                                           int top,
                                           int right,
                                           int bottom,
                                           t23_rgb8_t *out)
{
    double xs[2];
    double ys[2];
    unsigned int x_count = 0;
    unsigned int y_count = 0;
    unsigned int xi;
    unsigned int yi;
    unsigned int count = 0;
    unsigned int r_sum = 0;
    unsigned int g_sum = 0;
    unsigned int b_sum = 0;

    left = clamp_int(left, 0, model->crop_width - 1);
    right = clamp_int(right, 0, model->crop_width - 1);
    top = clamp_int(top, 0, model->crop_height - 1);
    bottom = clamp_int(bottom, 0, model->crop_height - 1);
    if (right < left) {
        right = left;
    }
    if (bottom < top) {
        bottom = top;
    }

    if (right - left >= 2) {
        xs[0] = (double)left + (double)(right - left) * 0.25;
        xs[1] = (double)left + (double)(right - left) * 0.75;
        x_count = 2;
    } else {
        xs[0] = ((double)left + (double)right) * 0.5;
        x_count = 1;
    }

    if (bottom - top >= 2) {
        ys[0] = (double)top + (double)(bottom - top) * 0.25;
        ys[1] = (double)top + (double)(bottom - top) * 0.75;
        y_count = 2;
    } else {
        ys[0] = ((double)top + (double)bottom) * 0.5;
        y_count = 1;
    }

    for (yi = 0; yi < y_count; ++yi) {
        for (xi = 0; xi < x_count; ++xi) {
            t23_rgb8_t sample;

            if (sample_rectified_rgb(src_rgb, src_w, src_h, model, xs[xi], ys[yi], &sample) < 0) {
                continue;
            }
            r_sum += sample.r;
            g_sum += sample.g;
            b_sum += sample.b;
            ++count;
        }
    }

    if (count == 0) {
        double center_x = ((double)left + (double)right) * 0.5;
        double center_y = ((double)top + (double)bottom) * 0.5;

        if (sample_rectified_rgb(src_rgb, src_w, src_h, model, center_x, center_y, out) == 0) {
            return;
        }
        out->r = 0;
        out->g = 0;
        out->b = 0;
        return;
    }

    out->r = (uint8_t)(r_sum / count);
    out->g = (uint8_t)(g_sum / count);
    out->b = (uint8_t)(b_sum / count);
}

static void compute_sparse_rectified_patch_nv12(const unsigned char *src_nv12,
                                                int src_w,
                                                int src_h,
                                                const rectification_model_t *model,
                                                int left,
                                                int top,
                                                int right,
                                                int bottom,
                                                t23_rgb8_t *out)
{
    double xs[2];
    double ys[2];
    unsigned int x_count = 0;
    unsigned int y_count = 0;
    unsigned int xi;
    unsigned int yi;
    unsigned int count = 0;
    unsigned int r_sum = 0;
    unsigned int g_sum = 0;
    unsigned int b_sum = 0;

    left = clamp_int(left, 0, model->crop_width - 1);
    right = clamp_int(right, 0, model->crop_width - 1);
    top = clamp_int(top, 0, model->crop_height - 1);
    bottom = clamp_int(bottom, 0, model->crop_height - 1);
    if (right < left) {
        right = left;
    }
    if (bottom < top) {
        bottom = top;
    }

    if (right - left >= 2) {
        xs[0] = (double)left + (double)(right - left) * 0.25;
        xs[1] = (double)left + (double)(right - left) * 0.75;
        x_count = 2;
    } else {
        xs[0] = ((double)left + (double)right) * 0.5;
        x_count = 1;
    }

    if (bottom - top >= 2) {
        ys[0] = (double)top + (double)(bottom - top) * 0.25;
        ys[1] = (double)top + (double)(bottom - top) * 0.75;
        y_count = 2;
    } else {
        ys[0] = ((double)top + (double)bottom) * 0.5;
        y_count = 1;
    }

    for (yi = 0; yi < y_count; ++yi) {
        for (xi = 0; xi < x_count; ++xi) {
            t23_rgb8_t sample;

            if (sample_rectified_rgb_nv12(src_nv12, src_w, src_h, model, xs[xi], ys[yi], &sample) < 0) {
                continue;
            }
            r_sum += sample.r;
            g_sum += sample.g;
            b_sum += sample.b;
            ++count;
        }
    }

    if (count == 0) {
        double center_x = ((double)left + (double)right) * 0.5;
        double center_y = ((double)top + (double)bottom) * 0.5;

        if (sample_rectified_rgb_nv12(src_nv12, src_w, src_h, model, center_x, center_y, out) == 0) {
            return;
        }
        out->r = 0;
        out->g = 0;
        out->b = 0;
        return;
    }

    out->r = (uint8_t)(r_sum / count);
    out->g = (uint8_t)(g_sum / count);
    out->b = (uint8_t)(b_sum / count);
}

static int build_preview_mosaic_from_calibration(const unsigned char *src_jpeg,
                                                 size_t src_jpeg_len,
                                                 unsigned char *rgb_out,
                                                 size_t rgb_out_capacity)
{
    unsigned char *src_rgb = NULL;
    rectification_model_t model;
    int src_w = 0;
    int src_h = 0;
    unsigned int cell_w;
    unsigned int cell_h;
    unsigned int x;
    unsigned int y;

    if (rgb_out == NULL || rgb_out_capacity < T23_C3_PREVIEW_MOSAIC_RGB_LEN) {
        return -1;
    }

    if (decode_jpeg_to_rgb888(src_jpeg, (unsigned long)src_jpeg_len, &src_rgb, &src_w, &src_h) < 0) {
        return -1;
    }

    if (build_rectification_model(&g_calibration, src_w, src_h, &model) < 0) {
        free(src_rgb);
        return -1;
    }

    cell_w = T23_C3_PREVIEW_MOSAIC_WIDTH;
    cell_h = T23_C3_PREVIEW_MOSAIC_HEIGHT;
    for (y = 0; y < cell_h; ++y) {
        int top = (int)((long long)y * model.crop_height / (int)cell_h);
        int bottom = (int)((long long)(y + 1u) * model.crop_height / (int)cell_h) - 1;

        if (bottom < top) {
            bottom = top;
        }
        for (x = 0; x < cell_w; ++x) {
            int left = (int)((long long)x * model.crop_width / (int)cell_w);
            int right = (int)((long long)(x + 1u) * model.crop_width / (int)cell_w) - 1;
            t23_rgb8_t color;
            size_t idx;

            if (right < left) {
                right = left;
            }
            compute_sparse_rectified_patch(src_rgb, src_w, src_h, &model, left, top, right, bottom, &color);
            idx = ((size_t)y * cell_w + x) * 3u;
            rgb_out[idx + 0u] = color.r;
            rgb_out[idx + 1u] = color.g;
            rgb_out[idx + 2u] = color.b;
        }
    }

    free(src_rgb);
    return 0;
}

static int build_preview_mosaic_from_nv12_buffer(const unsigned char *src_nv12,
                                                 int src_w,
                                                 int src_h,
                                                 unsigned char *rgb_out,
                                                 size_t rgb_out_capacity)
{
    const rectification_model_t *model = NULL;
    unsigned int cell_w;
    unsigned int cell_h;
    unsigned int x;
    unsigned int y;

    if (src_nv12 == NULL || rgb_out == NULL || rgb_out_capacity < T23_C3_PREVIEW_MOSAIC_RGB_LEN) {
        return -1;
    }
    if (src_w <= 1 || src_h <= 1) {
        return -1;
    }

    if (get_cached_rectification_model(src_w, src_h, &model) < 0) {
        return -1;
    }

    cell_w = T23_C3_PREVIEW_MOSAIC_WIDTH;
    cell_h = T23_C3_PREVIEW_MOSAIC_HEIGHT;
    for (y = 0; y < cell_h; ++y) {
        int top = (int)((long long)y * model->crop_height / (int)cell_h);
        int bottom = (int)((long long)(y + 1u) * model->crop_height / (int)cell_h) - 1;

        if (bottom < top) {
            bottom = top;
        }
        for (x = 0; x < cell_w; ++x) {
            int left = (int)((long long)x * model->crop_width / (int)cell_w);
            int right = (int)((long long)(x + 1u) * model->crop_width / (int)cell_w) - 1;
            t23_rgb8_t color;
            size_t idx;

            if (right < left) {
                right = left;
            }
            compute_sparse_rectified_patch_nv12(src_nv12, src_w, src_h, model, left, top, right, bottom, &color);
            idx = ((size_t)y * cell_w + x) * 3u;
            rgb_out[idx + 0u] = color.r;
            rgb_out[idx + 1u] = color.g;
            rgb_out[idx + 2u] = color.b;
        }
    }

    return 0;
}

/*
 * Compute all border block averages directly from the original JPEG frame by
 * inverse mapping through the saved calibration model. This is the optimized
 * runtime path used by RUN mode, and avoids building a full rectified image for
 * every frame.
 */
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
    unsigned char *src_rgb = NULL;
    rectification_model_t model;
    int src_w = 0;
    int src_h = 0;
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

    if (decode_jpeg_to_rgb888(src_jpeg, (unsigned long)src_jpeg_len, &src_rgb, &src_w, &src_h) < 0) {
        return -1;
    }

    if (build_rectification_model(&g_calibration, src_w, src_h, &model) < 0) {
        free(src_rgb);
        return -1;
    }

    image_w = model.crop_width;
    image_h = model.crop_height;
    rect_left = 0;
    rect_top = 0;
    rect_right = image_w - 1;
    rect_bottom = image_h - 1;

    rect_w = rect_right - rect_left + 1;
    rect_h = rect_bottom - rect_top + 1;
    thickness = clamp_int((rect_w < rect_h ? rect_w : rect_h) / 10, 2, rect_h / 2 > 0 ? rect_h / 2 : 2);

    for (i = 0; i < layout->top_blocks; ++i) {
        int x0 = rect_left + (int)((long long)rect_w * i / layout->top_blocks);
        int x1 = rect_left + (int)((long long)rect_w * (i + 1) / layout->top_blocks) - 1;

        blocks[idx].block_index = (uint8_t)idx;
        compute_average_rectified_patch(src_rgb, src_w, src_h, &model, x0, rect_top, x1, rect_top + thickness - 1, &blocks[idx].color);
        ++idx;
    }

    for (i = 0; i < layout->right_blocks; ++i) {
        int y0 = rect_top + (int)((long long)rect_h * i / layout->right_blocks);
        int y1 = rect_top + (int)((long long)rect_h * (i + 1) / layout->right_blocks) - 1;

        blocks[idx].block_index = (uint8_t)idx;
        compute_average_rectified_patch(src_rgb, src_w, src_h, &model, rect_right - thickness + 1, y0, rect_right, y1, &blocks[idx].color);
        ++idx;
    }

    for (i = 0; i < layout->bottom_blocks; ++i) {
        int x0 = rect_left + (int)((long long)rect_w * (layout->bottom_blocks - 1 - i) / layout->bottom_blocks);
        int x1 = rect_left + (int)((long long)rect_w * (layout->bottom_blocks - i) / layout->bottom_blocks) - 1;

        blocks[idx].block_index = (uint8_t)idx;
        compute_average_rectified_patch(src_rgb, src_w, src_h, &model, x0, rect_bottom - thickness + 1, x1, rect_bottom, &blocks[idx].color);
        ++idx;
    }

    for (i = 0; i < layout->left_blocks; ++i) {
        int y0 = rect_top + (int)((long long)rect_h * (layout->left_blocks - 1 - i) / layout->left_blocks);
        int y1 = rect_top + (int)((long long)rect_h * (layout->left_blocks - i) / layout->left_blocks) - 1;

        blocks[idx].block_index = (uint8_t)idx;
        compute_average_rectified_patch(src_rgb, src_w, src_h, &model, rect_left, y0, rect_left + thickness - 1, y1, &blocks[idx].color);
        ++idx;
    }

    free(src_rgb);
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

/*
 * Capture the latest image, compute the current rectified 16x16 mosaic, and
 * push it over SPI. RUN mode keeps UART only for control/mode switching, while
 * all high-rate image transport goes through SPI.
 */
static int push_runtime_mosaic_from_current(void)
{
    IMPFrameInfo frame;
    IMPFSChnAttr fs_attr;
    int fs_chn = get_active_framesource_channel();
    int ret = -1;
    int src_w = 0;
    int src_h = 0;
    unsigned char mosaic_rgb[T23_C3_PREVIEW_MOSAIC_RGB_LEN];

    if (fs_chn < 0) {
        return -1;
    }

    memset(&frame, 0, sizeof(frame));
    memset(&fs_attr, 0, sizeof(fs_attr));
    if (IMP_FrameSource_GetChnAttr(fs_chn, &fs_attr) < 0) {
        return -1;
    }
    src_w = fs_attr.scaler.enable ? fs_attr.scaler.outwidth : fs_attr.picWidth;
    src_h = fs_attr.scaler.enable ? fs_attr.scaler.outheight : fs_attr.picHeight;
    if (src_w <= 1 || src_h <= 1) {
        return -1;
    }
    if (((size_t)src_w * (size_t)src_h * 3u / 2u) > sizeof(g_jpeg_buf)) {
        return -1;
    }

    if (IMP_FrameSource_SnapFrame(fs_chn, PIX_FMT_NV12, src_w, src_h, g_jpeg_buf, &frame) < 0) {
        return -1;
    }

    if (build_preview_mosaic_from_nv12_buffer(g_jpeg_buf, src_w, src_h, mosaic_rgb, sizeof(mosaic_rgb)) == 0) {
        ret = push_runtime_preview_mosaic_over_spi(mosaic_rgb, sizeof(mosaic_rgb));
    }

    return ret;
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

static int get_active_framesource_channel(void)
{
    unsigned int i;

    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            return (int)chn[i].index;
        }
    }

    return -1;
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

static int startup_pipeline_with_cfg(const sample_sensor_cfg_t *sensor_cfg)
{
    if (sample_system_init(*sensor_cfg) < 0) {
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

static int startup_pipeline(void)
{
    sample_sensor_cfg_t sensor_cfg = make_sensor_cfg();

    return startup_pipeline_with_cfg(&sensor_cfg);
}

/*
 * Release IMP pipeline resources in reverse order. Keeping teardown in one
 * place makes shutdown and mode switching behavior easier to follow.
 */
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
    int flags;
    struct termios tio;

    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
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

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
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

/*
 * DEBUG mode still uses the original line-oriented UART command protocol.
 * Polling through a persistent buffer lets the process stay responsive without
 * blocking the runtime loop on every read.
 */
static int poll_serial_commands_nonblocking(int fd, char *buf, size_t buf_size, size_t *len_io)
{
    size_t len = *len_io;

    while (g_running) {
        char ch;
        ssize_t ret = read(fd, &ch, 1);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return -1;
        }
        if (ret == 0) {
            break;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (len > 0) {
                buf[len] = '\0';
                printf("bridge rx: %s\n", buf);
                process_command(fd, buf);
                len = 0;
            }
            continue;
        }
        if (len + 1 < buf_size) {
            buf[len++] = ch;
        } else {
            len = 0;
        }
    }

    *len_io = len;
    return 0;
}

/*
 * High-speed RUN loop:
 * - opportunistically service incoming ASCII control commands
 * - if mode is still RUN, compute one 16x16 mosaic from the latest image
 * - immediately push that mosaic to the C3 over SPI
 *
 * This removes the older request/response latency from the runtime path.
 */
static void run_mode_loop(int fd)
{
    char cmd_buf[UART_RX_BUF_SIZE];
    size_t cmd_len = 0;
    int stream_failures = 0;

    memset(cmd_buf, 0, sizeof(cmd_buf));

    while (g_running && g_bridge_mode == BRIDGE_MODE_RUN) {
        if (poll_serial_commands_nonblocking(fd, cmd_buf, sizeof(cmd_buf), &cmd_len) < 0) {
            perror("poll_serial_commands_nonblocking");
            break;
        }
        if (!g_running || g_bridge_mode != BRIDGE_MODE_RUN) {
            break;
        }

        if (push_runtime_mosaic_from_current() < 0) {
            ++stream_failures;
            if (stream_failures < 5 || (stream_failures % 20) == 0) {
                fprintf(stderr, "run_mode stream frame failed (%d)\n", stream_failures);
            }
            usleep(5000);
        } else {
            stream_failures = 0;
        }

        if (poll_serial_commands_nonblocking(fd, cmd_buf, sizeof(cmd_buf), &cmd_len) < 0) {
            perror("poll_serial_commands_nonblocking");
            break;
        }
    }
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

static int get_hue_value(int *value)
{
    unsigned char v = 0;
    int ret = IMP_ISP_Tuning_GetBcshHue(&v);
    *value = (int)v;
    return ret;
}

static int set_hue_value(int value)
{
    return IMP_ISP_Tuning_SetBcshHue((unsigned char)value);
}

static int get_hilight_value(int *value)
{
    unsigned int v = 0;
    int ret = IMP_ISP_Tuning_GetHiLightDepress(&v);
    *value = (int)v;
    return ret;
}

static int set_hilight_value(int value)
{
    return IMP_ISP_Tuning_SetHiLightDepress((unsigned int)value);
}

static int get_backlight_value(int *value)
{
    unsigned int v = 0;
    int ret = IMP_ISP_Tuning_GetBacklightComp(&v);
    *value = (int)v;
    return ret;
}

static int set_backlight_value(int value)
{
    return IMP_ISP_Tuning_SetBacklightComp((unsigned int)value);
}

static int get_temper_value(int *value)
{
    *value = g_temper_value_shadow;
    return 0;
}

static int set_temper_value(int value)
{
    int ret = IMP_ISP_Tuning_SetTemperStrength((unsigned int)value);
    if (ret == 0) {
        g_temper_value_shadow = value;
    }
    return ret;
}

static int get_sinter_value(int *value)
{
    *value = g_sinter_value_shadow;
    return 0;
}

static int set_sinter_value(int value)
{
    int ret = IMP_ISP_Tuning_SetSinterStrength((unsigned int)value);
    if (ret == 0) {
        g_sinter_value_shadow = value;
    }
    return ret;
}

static bridge_param_desc_t g_params[] = {
    { "BRIGHTNESS", T23_C3_PARAM_BRIGHTNESS, get_brightness_value, set_brightness_value, 0, 255 },
    { "CONTRAST", T23_C3_PARAM_CONTRAST, get_contrast_value, set_contrast_value, 0, 255 },
    { "SHARPNESS", T23_C3_PARAM_SHARPNESS, get_sharpness_value, set_sharpness_value, 0, 255 },
    { "SATURATION", T23_C3_PARAM_SATURATION, get_saturation_value, set_saturation_value, 0, 255 },
    { "AE_COMP", T23_C3_PARAM_AE_COMP, get_ae_comp_value, set_ae_comp_value, 90, 250 },
    { "DRC", T23_C3_PARAM_DRC, get_drc_value, set_drc_value, 0, 255 },
    { "AWB_CT", T23_C3_PARAM_AWB_CT, get_awb_ct_value, set_awb_ct_value, 1500, 12000 },
    { "HUE", T23_C3_PARAM_HUE, get_hue_value, set_hue_value, 0, 255 },
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
    char line[256];
    size_t len = 0;
    unsigned int i;

    sendf(fd,
          "CAL SIZE %u %u",
          (unsigned int)g_calibration.image_width,
          (unsigned int)g_calibration.image_height);

    len = (size_t)snprintf(line, sizeof(line), "CAL POINTS");
    for (i = 0; i < T23_BORDER_POINT_COUNT && len + 24 < sizeof(line); ++i) {
        len += (size_t)snprintf(line + len,
                                sizeof(line) - len,
                                " %d %d",
                                g_calibration.points[i].x,
                                g_calibration.points[i].y);
    }
    send_line(fd, line);
    send_line(fd, "OK CAL GET");
}

static void send_lens_status(int fd)
{
    sendf(fd,
          "LENS PRESET %d %d %d %d %d %d %d %d",
          1,
          (int)lrint(FISHEYE_FX * 1000.0),
          (int)lrint(FISHEYE_FY * 1000.0),
          (int)lrint(FISHEYE_CX * 1000.0),
          (int)lrint(FISHEYE_CY * 1000.0),
          (int)lrint(FISHEYE_K1 * 1000000.0),
          (int)lrint(FISHEYE_K2 * 1000000.0),
          (int)lrint(FISHEYE_KNEW_SCALE * 1000.0));
    send_line(fd, "OK LENS GET");
}

static void handle_cal_set(int fd, char *args)
{
    t23_border_calibration_t cal;
    int values[2 + T23_BORDER_POINT_COUNT * 2];
    char *token;
    char *saveptr = NULL;
    int i;

    memset(&cal, 0, sizeof(cal));
    for (i = 0; i < (int)(sizeof(values) / sizeof(values[0])); ++i) {
        token = strtok_r((i == 0) ? args : NULL, " ", &saveptr);
        if (token == NULL) {
            send_line(fd, "ERR CAL_SET_FORMAT");
            return;
        }
        values[i] = atoi(token);
    }
    if (strtok_r(NULL, " ", &saveptr) != NULL) {
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
    invalidate_runtime_rectification_cache();

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

static int capture_hires_jpeg_once(unsigned char *out_buf, size_t *out_len)
{
    sample_sensor_cfg_t hi_cfg = make_sensor_cfg_with_size(HIRES_SNAPSHOT_WIDTH, HIRES_SNAPSHOT_HEIGHT);
    int ret = -1;

    /*
     * High-resolution snapshots are intentionally handled as a one-shot mode
     * switch. This keeps the normal 640x320 preview path low-latency while
     * still letting calibration capture a genuinely higher resolution frame.
     */
    shutdown_pipeline();
    if (startup_pipeline_with_cfg(&hi_cfg) == 0) {
        ret = capture_jpeg_once(out_buf, out_len);
    }
    shutdown_pipeline();
    if (startup_pipeline() < 0) {
        fprintf(stderr, "failed to restore preview pipeline after hi-res snapshot\n");
        g_running = 0;
    }

    return ret;
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

static int push_one_spi_frame_with_timeout(const t23_c3_frame_t *frame, int timeout_ms)
{
    t23_c3_frame_t rx_frame;
    int ret;

    ret = wait_data_ready_high(timeout_ms);
    if (ret != 0) {
        return -1;
    }

    memset(&rx_frame, 0, sizeof(rx_frame));
    return spi_transfer_frame(frame, &rx_frame);
}

static int push_one_spi_frame(const t23_c3_frame_t *frame)
{
    return push_one_spi_frame_with_timeout(frame, SPI_DATA_READY_TIMEOUT_MS);
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

static int push_preview_mosaic_over_spi(const unsigned char *rgb_buf, size_t rgb_len)
{
    t23_c3_frame_t frame;

    if (rgb_buf == NULL || rgb_len == 0 || rgb_len > T23_C3_FRAME_PAYLOAD_MAX) {
        return -1;
    }

    fill_frame_header(&frame,
                      T23_C3_FRAME_TYPE_RESP_MOSAIC_RGB,
                      1,
                      T23_C3_PARAM_NONE,
                      T23_C3_STATUS_OK,
                      (uint16_t)rgb_len,
                      (uint32_t)rgb_len,
                      0);
    memcpy(frame.payload, rgb_buf, rgb_len);
    return push_one_spi_frame(&frame);
}

static int push_runtime_preview_mosaic_over_spi(const unsigned char *rgb_buf, size_t rgb_len)
{
    t23_c3_frame_t frame;

    if (rgb_buf == NULL || rgb_len == 0 || rgb_len > T23_C3_FRAME_PAYLOAD_MAX) {
        return -1;
    }

    fill_frame_header(&frame,
                      T23_C3_FRAME_TYPE_RESP_MOSAIC_RGB,
                      1,
                      T23_C3_PARAM_NONE,
                      T23_C3_STATUS_OK,
                      (uint16_t)rgb_len,
                      (uint32_t)rgb_len,
                      0);
    memcpy(frame.payload, rgb_buf, rgb_len);
    return push_one_spi_frame_with_timeout(&frame, RUN_SPI_DATA_READY_TIMEOUT_MS);
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

static void handle_snap_hires(int fd)
{
    size_t jpeg_len = 0;

    if (capture_hires_jpeg_once(g_jpeg_buf, &jpeg_len) < 0) {
        send_line(fd, "ERR SNAP_HIRES");
        return;
    }

    sendf(fd, "SNAP HIRES OK %u", (unsigned int)jpeg_len);
    if (push_jpeg_over_spi(g_jpeg_buf, jpeg_len) < 0) {
        send_line(fd, "ERR SNAP_HIRES_SPI");
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

static void handle_cal_mosaic(int fd)
{
    size_t jpeg_len = 0;
    unsigned char mosaic_rgb[T23_C3_PREVIEW_MOSAIC_RGB_LEN];

    if (capture_jpeg_once(g_jpeg_buf, &jpeg_len) < 0) {
        send_line(fd, "ERR CAL_MOSAIC");
        return;
    }

    if (build_preview_mosaic_from_calibration(g_jpeg_buf, jpeg_len, mosaic_rgb, sizeof(mosaic_rgb)) < 0) {
        send_line(fd, "ERR CAL_MOSAIC_BUILD");
        return;
    }

    sendf(fd,
          "CAL MOSAIC OK %u %u %u",
          (unsigned int)T23_C3_PREVIEW_MOSAIC_WIDTH,
          (unsigned int)T23_C3_PREVIEW_MOSAIC_HEIGHT,
          (unsigned int)sizeof(mosaic_rgb));
    if (push_preview_mosaic_over_spi(mosaic_rgb, sizeof(mosaic_rgb)) < 0) {
        send_line(fd, "ERR CAL_MOSAIC_SPI");
    }
}

static int undistort_jpeg_with_fixed_fisheye(const unsigned char *src_jpeg,
                                             size_t src_jpeg_len,
                                             unsigned char *out_jpeg,
                                             size_t out_jpeg_capacity,
                                             size_t *out_jpeg_len)
{
    unsigned char *src_rgb = NULL;
    unsigned char *dst_rgb = NULL;
    unsigned char *jpeg_mem = NULL;
    size_t jpeg_size = 0;
    rectification_model_t model;
    int src_w = 0;
    int src_h = 0;
    int x;
    int y;

    if (decode_jpeg_to_rgb888(src_jpeg, (unsigned long)src_jpeg_len, &src_rgb, &src_w, &src_h) < 0) {
        return -1;
    }

    memset(&model, 0, sizeof(model));
    if (init_calibration_fisheye_profile(&model, src_w, src_h) < 0) {
        free(src_rgb);
        return -1;
    }

    dst_rgb = malloc((size_t)src_w * src_h * 3);
    if (dst_rgb == NULL) {
        free(src_rgb);
        return -1;
    }

    for (y = 0; y < src_h; ++y) {
        for (x = 0; x < src_w; ++x) {
            pointf_t undist_pt;
            pointf_t src_pt;
            unsigned char *pixel = dst_rgb + (y * src_w + x) * 3;

            undist_pt.x = (double)x;
            undist_pt.y = (double)y;
            if (distort_point_with_fisheye(&model, &undist_pt, &src_pt) < 0) {
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                continue;
            }

            pixel[0] = bilinear_sample_channel(src_rgb, src_w, src_h, src_pt.x, src_pt.y, 0);
            pixel[1] = bilinear_sample_channel(src_rgb, src_w, src_h, src_pt.x, src_pt.y, 1);
            pixel[2] = bilinear_sample_channel(src_rgb, src_w, src_h, src_pt.x, src_pt.y, 2);
        }
    }

    if (encode_rgb888_to_jpeg(dst_rgb, src_w, src_h, &jpeg_mem, &jpeg_size) < 0) {
        free(dst_rgb);
        free(src_rgb);
        return -1;
    }

    if (jpeg_size == 0 || jpeg_size > out_jpeg_capacity) {
        free(jpeg_mem);
        free(dst_rgb);
        free(src_rgb);
        return -1;
    }

    memcpy(out_jpeg, jpeg_mem, jpeg_size);
    *out_jpeg_len = jpeg_size;

    free(jpeg_mem);
    free(dst_rgb);
    free(src_rgb);
    return 0;
}

static void handle_cal_undistorted_snap(int fd)
{
    size_t jpeg_len = 0;
    size_t undistorted_len = 0;

    if (capture_jpeg_once(g_jpeg_buf, &jpeg_len) < 0) {
        send_line(fd, "ERR CAL_USNAP");
        return;
    }

    if (undistort_jpeg_with_fixed_fisheye(g_jpeg_buf, jpeg_len, g_jpeg_buf, sizeof(g_jpeg_buf), &undistorted_len) < 0) {
        send_line(fd, "ERR CAL_UNDISTORT");
        return;
    }

    sendf(fd, "CAL USNAP OK %u", (unsigned int)undistorted_len);
    if (push_jpeg_over_spi(g_jpeg_buf, undistorted_len) < 0) {
        send_line(fd, "ERR CAL_USNAP_SPI");
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
        send_line(fd, "INFO commands: PING, HELP, MODE GET|SET <DEBUG|RUN>, LAYOUT GET|SET <16X9|4X3>, GET <PARAM|ALL>, SET <PARAM> <VALUE>, SNAP [HIRES], FRAME, CAL GET, CAL SET, CAL SNAP, CAL MOSAIC, CAL USNAP, LENS GET, BLOCKS GET");
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

    if (strcmp(cmd, "LENS") == 0) {
        arg1 = strtok_r(NULL, " ", &saveptr);
        if (arg1 == NULL) {
            send_line(fd, "ERR missing-lens-subcommand");
            return;
        }
        strtoupper(arg1);

        if (strcmp(arg1, "GET") == 0) {
            send_lens_status(fd);
            return;
        }

        send_line(fd, "ERR unknown-lens-subcommand");
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
        arg1 = strtok_r(NULL, " ", &saveptr);
        if (arg1 != NULL) {
            strtoupper(arg1);
            if (strcmp(arg1, "HIRES") == 0) {
                handle_snap_hires(fd);
                return;
            }
        }
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

        if (strcmp(arg1, "MOSAIC") == 0) {
            handle_cal_mosaic(fd);
            return;
        }

        if (strcmp(arg1, "USNAP") == 0) {
            handle_cal_undistorted_snap(fd);
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
    char cmd_buf[UART_RX_BUF_SIZE];
    size_t cmd_len = 0;

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
    memset(cmd_buf, 0, sizeof(cmd_buf));

    while (g_running) {
        if (g_bridge_mode == BRIDGE_MODE_RUN) {
            run_mode_loop(serial_fd);
            continue;
        }
        if (poll_serial_commands_nonblocking(serial_fd, cmd_buf, sizeof(cmd_buf), &cmd_len) < 0) {
            perror("poll_serial_commands_nonblocking");
            break;
        }
        usleep(10000);
    }

    close(serial_fd);
    close_transport_resources();
    shutdown_pipeline();
    return 0;
}
