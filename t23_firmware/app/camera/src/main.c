#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "camera_common.h"

#include <imp/imp_encoder.h>
#include <imp/imp_system.h>

extern struct chn_conf chn[];

/*
 * T23 camera diagnostic entry point.
 *
 * The program is intentionally split into staged modes so serial logs can tell
 * us exactly which layer is alive after flashing a new image:
 * - isp-only: sensor + ISP only
 * - framesource: local video pipeline
 * - jpeg: codec/VPU path on top of the video pipeline
 */
typedef enum {
    MODE_ISP_ONLY = 0,
    MODE_FRAMESOURCE,
    MODE_JPEG,
} diag_mode_t;

/**
 * @brief Print command-line usage for the camera diagnostic tool.
 *
 * @param prog Program name displayed in the help text.
 */
static void usage(const char *prog)
{
    printf("Usage: %s [isp-only|framesource|jpeg]\n", prog);
    printf("  isp-only    : init sensor + ISP only\n");
    printf("  framesource : init sensor + ISP + framesource and stream on briefly\n");
    printf("  jpeg        : add JPEG encoder path to isolate VPU/hwicodec issues\n");
}

/**
 * @brief Build the runtime sensor configuration used by all diagnostic modes.
 *
 * @return Fully initialized sensor configuration structure.
 */
static sample_sensor_cfg_t make_sensor_cfg(void)
{
    sample_sensor_cfg_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    /* Keep all modes on the same sensor config to avoid hidden differences. */
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

/**
 * @brief Create encoder groups for all enabled channels.
 *
 * @return 0 on success, -1 on failure.
 */
static int create_groups(void)
{
    int i;
    int ret;

    /* Encoder channels must belong to groups before JPEG init can succeed. */
    for (i = 0; i < FS_CHN_NUM; i++) {
        if (!chn[i].enable) {
            continue;
        }
        ret = IMP_Encoder_CreateGroup(chn[i].index);
        if (ret < 0) {
            printf("diag: IMP_Encoder_CreateGroup(%d) failed: %d\n", chn[i].index, ret);
            return -1;
        }
    }

    return 0;
}

/**
 * @brief Destroy the encoder groups created for JPEG mode.
 */
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

/**
 * @brief Bind frame-source channels to the JPEG encoder path.
 *
 * @return 0 on success, -1 on failure.
 */
static int bind_jpeg_channels(void)
{
    int i;
    int ret;

    /* Connect frame-source output directly into the JPEG encoder input. */
    for (i = 0; i < FS_CHN_NUM; i++) {
        if (!chn[i].enable) {
            continue;
        }
        ret = IMP_System_Bind(&chn[i].framesource_chn, &chn[i].imp_encoder);
        if (ret < 0) {
            printf("diag: IMP_System_Bind(channel %d) failed: %d\n", chn[i].index, ret);
            return -1;
        }
    }

    return 0;
}

/**
 * @brief Remove the frame-source to encoder bindings created for JPEG mode.
 */
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

/**
 * @brief Parse the requested diagnostic mode from the command line.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 *
 * @return Selected diagnostic mode.
 */
static diag_mode_t parse_mode(int argc, char *argv[])
{
    /* Framesource is the most useful default daily bring-up test. */
    if (argc < 2) {
        return MODE_FRAMESOURCE;
    }
    if (strcmp(argv[1], "isp-only") == 0) {
        return MODE_ISP_ONLY;
    }
    if (strcmp(argv[1], "framesource") == 0) {
        return MODE_FRAMESOURCE;
    }
    if (strcmp(argv[1], "jpeg") == 0) {
        return MODE_JPEG;
    }
    usage(argv[0]);
    exit(2);
}

/**
 * @brief Entry point for the staged T23 camera diagnostic program.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 *
 * @return 0 on success, non-zero on failure.
 */
int main(int argc, char *argv[])
{
    int ret;
    diag_mode_t mode = parse_mode(argc, argv);
    sample_sensor_cfg_t sensor_cfg = make_sensor_cfg();

    printf("=====================================\n");
    printf("t23_camera_diag start\n");
    printf("sensor=%s i2c_bus=%u addr=0x%02x size=%dx%d\n",
           sensor_cfg.sensor_name,
           sensor_cfg.sensor_i2c_adapter_id,
           sensor_cfg.sensor_i2c_addr,
           sensor_cfg.sensor_width,
           sensor_cfg.sensor_height);
    printf("mode=%s\n",
           mode == MODE_ISP_ONLY ? "isp-only" :
           mode == MODE_FRAMESOURCE ? "framesource" : "jpeg");
    printf("=====================================\n");

    ret = sample_system_init(sensor_cfg);
    if (ret < 0) {
        printf("diag: sample_system_init failed\n");
        return 1;
    }
    printf("diag: sample_system_init ok\n");

    if (mode == MODE_ISP_ONLY) {
        printf("diag: ISP-only mode complete\n");
        sample_system_exit();
        return 0;
    }

    ret = sample_framesource_init();
    if (ret < 0) {
        printf("diag: sample_framesource_init failed\n");
        /* Nothing above framesource matters until this layer is healthy. */
        sample_system_exit();
        return 1;
    }
    printf("diag: sample_framesource_init ok\n");

    if (mode == MODE_FRAMESOURCE) {
        ret = sample_framesource_streamon();
        if (ret < 0) {
            printf("diag: sample_framesource_streamon failed\n");
            sample_framesource_exit();
            sample_system_exit();
            return 1;
        }
        printf("diag: framesource stream on, waiting %d seconds\n", SLEEP_TIME);
        sleep(SLEEP_TIME);
        sample_framesource_streamoff();
        sample_framesource_exit();
        sample_system_exit();
        printf("diag: framesource mode complete\n");
        return 0;
    }

    ret = create_groups();
    if (ret < 0) {
        sample_framesource_exit();
        sample_system_exit();
        return 1;
    }
    printf("diag: encoder groups created\n");

    ret = sample_jpeg_init();
    if (ret < 0) {
        printf("diag: sample_jpeg_init failed\n");
        printf("diag: this points to JPEG/VPU/hwicodec path, not basic sensor/ISP bring-up\n");
        destroy_groups();
        sample_framesource_exit();
        sample_system_exit();
        return 1;
    }
    printf("diag: sample_jpeg_init ok\n");

    ret = bind_jpeg_channels();
    if (ret < 0) {
        sample_encoder_exit();
        destroy_groups();
        sample_framesource_exit();
        sample_system_exit();
        return 1;
    }
    printf("diag: JPEG channels bound\n");

    ret = sample_framesource_streamon();
    if (ret < 0) {
        printf("diag: sample_framesource_streamon failed in jpeg mode\n");
        unbind_jpeg_channels();
        sample_encoder_exit();
        destroy_groups();
        sample_framesource_exit();
        sample_system_exit();
        return 1;
    }

    sleep(SLEEP_TIME);
    /* Save a small batch of JPEG snapshots into /tmp on the target. */
    ret = sample_get_jpeg_snap();
    printf("diag: sample_get_jpeg_snap ret=%d\n", ret);

    sample_framesource_streamoff();
    unbind_jpeg_channels();
    sample_encoder_exit();
    destroy_groups();
    sample_framesource_exit();
    sample_system_exit();

    printf("diag: jpeg mode complete\n");
    return ret < 0 ? 1 : 0;
}
