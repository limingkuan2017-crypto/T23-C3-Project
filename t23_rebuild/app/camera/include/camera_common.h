/*
 * camera_common.h
 *
 * This header is derived from the vendor sample, but is now used by the
 * rebuild diagnostics project. It collects:
 * 1. active sensor selection and geometry
 * 2. common media pipeline constants
 * 3. shared configuration structures
 * 4. helper API declarations exported by camera_common.c
 *
 * For a new developer this file acts like a "static map" of the T23 camera
 * side. Before reading the larger implementation, read this file to answer:
 * - Which sensor is currently selected?
 * - What resolution and channels are enabled?
 * - Which helper functions exist in the common media layer?
 *
 * Copyright (C) 2014 Ingenic Semiconductor Co.,Ltd
 */

#ifndef __CAMERA_COMMON_H__
#define __CAMERA_COMMON_H__

#include <imp/imp_common.h>
#include <imp/imp_osd.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <unistd.h>

#ifdef __cplusplus
#if __cplusplus
extern "C"
{
#endif
#endif /* __cplusplus */

/* Sensor frame rate expressed as NUM / DEN. */
#define SENSOR_FRAME_RATE_NUM        25
#define SENSOR_FRAME_RATE_DEN        1

/*
 * Active sensor for the current board.
 *
 * The rebuild project keeps many vendor sensor definitions so the file remains
 * structurally close to the original sample, but only one SENSOR_* macro
 * should be enabled for a given build.
 *
 * Current board selection:
 * - sensor driver: sc2337p
 * - bus: i2c0
 * - realtime preview size: 640x320
 *
 * Why this is intentionally small:
 * - the product goal is not full-frame recording
 * - we only need stable, low-latency color sampling around the TV border
 * - smaller preview frames reduce JPEG encode time, SPI transfer time and C3
 *   buffering pressure
 *
 * A larger one-shot calibration/snapshot path can still be added later for:
 * - manual border picking
 * - lens/distortion calibration
 * - debugging image quality in more detail
 */
#define SENSOR_SC2337P
/* #define SENSOR_SC1346 */
/* #define SENSOR_GC2053 */
/* #define SECSENSOR */

#if defined SENSOR_AR0141
#define SENSOR_NAME                 "ar0141"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x10
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1280
#define SENSOR_HEIGHT               720
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_OV7725
#define SENSOR_NAME                 "ov7725"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x21
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                640
#define SENSOR_HEIGHT               480
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_OV9732
#define SENSOR_NAME                 "ov9732"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x36
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1280
#define SENSOR_HEIGHT               720
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_OV9750
#define SENSOR_NAME                 "ov9750"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x36
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1280
#define SENSOR_HEIGHT               720
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_OV9712
#define SENSOR_NAME                 "ov9712"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x30
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1280
#define SENSOR_HEIGHT               720
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_GC1004
#define SENSOR_NAME                 "gc1004"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x3c
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1280
#define SENSOR_HEIGHT               720
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_JXH42
#define SENSOR_NAME                 "jxh42"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x30
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1280
#define SENSOR_HEIGHT               720
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_SC1035
#define SENSOR_NAME                 "sc1035"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x30
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1280
#define SENSOR_HEIGHT               960
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_OV2710
#define SENSOR_NAME                 "ov2710"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x36
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_OV2735
#define SENSOR_NAME                 "ov2735"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x3c
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_OV2735B
#define SENSOR_NAME                 "ov2735b"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x3c
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_SC2135
#define SENSOR_NAME                 "sc2135"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x30
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_JXF22
#define SENSOR_NAME                 "jxf22"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x40
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_JXF23
#define SENSOR_NAME                 "jxf23"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x40
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     0
#define CHN1_EN                     1
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_JXF28
#define SENSOR_NAME                 "jxf28"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x40
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     1
#define CHN3_EN                     0
#define CROP_EN                     1
#elif defined SENSOR_SC2310
#define SENSOR_NAME                 "sc2310"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x30
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_SC2337P
#define SENSOR_NAME                 "sc2337p"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x30
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                640 /* Balanced preview size: larger than the low-latency experiment, still smaller than full sensor output. */
#define SENSOR_HEIGHT               320 /* Matches the previously stable preview path used during WiFi tuning. */
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_SC1346
#define SENSOR_NAME                 "sc1346"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x30
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                640 /* Original full width was 1920. */
#define SENSOR_HEIGHT               320 /* Original full height was 1080. */
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_GC2063
#define SENSOR_NAME                 "gc2063"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x37
#define SENSOR_I2C_ADAPTER_ID       1
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_GC2053
#define SENSOR_NAME                 "gc2053"
#define SENSOR_NAMES1               "gc2053s1"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x37
#define SENSOR_I2C_ADDRS1           0x3f
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_IMX327
#define SENSOR_NAME                 "imx327"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x1a
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_JXF37
#define SENSOR_NAME                 "jxf37"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x40
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_SC200AI
#define SENSOR_NAME                 "sc200ai"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x30
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                1920
#define SENSOR_HEIGHT               1080
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#elif defined SENSOR_SC301IoT
#define SENSOR_NAME                 "sc301IoT"
#define SENSOR_CUBS_TYPE            TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR             0x30
#define SENSOR_I2C_ADAPTER_ID       0
#define SENSOR_WIDTH                2048
#define SENSOR_HEIGHT               1536
#define CHN0_EN                     1
#define CHN1_EN                     0
#define CHN2_EN                     0
#define CHN3_EN                     0
#define CROP_EN                     0
#endif

/* Secondary stream size retained for compatibility with helper code. */
#define SENSOR_WIDTH_SECOND         640
#define SENSOR_HEIGHT_SECOND        360

/* Third stream size retained for compatibility with helper code. */
#define SENSOR_WIDTH_THIRD          1280
#define SENSOR_HEIGHT_THIRD         720

/* Reference bitrate for 720p sample encode paths. */
#define BITRATE_720P_Kbs            1000

/* Number of files produced by long-running sample helper routines. */
#define NR_FRAMES_TO_SAVE           200
#define NR_JPEG_TO_SAVE             20

/* Temporary buffer size used while dumping encoded stream data. */
#define STREAM_BUFFER_SIZE          (1 * 1024 * 1024)

/* Encoder channel IDs used by the simplified rebuild diagnostics. */
#define ENC_VIDEO_CHANNEL           0
#define ENC_JPEG_CHANNEL            1

/*
 * Keep diagnostics independent from the writable data partition. During early
 * bring-up we save everything to /tmp so JPEG tests can pass even if mtd5 is
 * unformatted or intentionally unused.
 */
#define STREAM_FILE_PATH_PREFIX     "/tmp"
#define SNAP_FILE_PATH_PREFIX       "/tmp"

/* OSD bitmap cell sizes retained from the vendor sample implementation. */
#define OSD_REGION_WIDTH            16
#define OSD_REGION_HEIGHT           34
#define OSD_REGION_WIDTH_SEC        8
#define OSD_REGION_HEIGHT_SEC       18

/* Sleep interval used by simple polling loops in the sample code. */
#define SLEEP_TIME                  1

/* Maximum number of frame-source channels in the helper configuration array. */
#define FS_CHN_NUM                  4
#define IVS_CHN_ID                  2

/* Human-readable aliases for frame-source channel indexes. */
#define CH0_INDEX                   0
#define CH1_INDEX                   1
#define CH2_INDEX                   2
#define CH3_INDEX                   3

/* Boolean-style channel enable flags used in configuration tables. */
#define CHN_ENABLE                  1
#define CHN_DISABLE                 0

/* #define SUPPORT_RGB555LE */

/**
 * @brief Per-channel description used by the media helper code.
 *
 * Each enabled entry describes one logical branch in the IMP media graph:
 * - fs_chn_attr defines width, height, pixel format, crop and scaling
 * - framesource_chn is the source-side IMP cell
 * - imp_encoder is the encoder-side IMP cell used for bind/unbind operations
 *
 * The rebuild diagnostics use only a subset of this table today, but the
 * structure is intentionally preserved because it matches the proven vendor
 * sample pipeline and makes future extension easier.
 */
struct chn_conf {
    unsigned int index;         /* 0 = main channel, 1 = second channel, etc. */
    unsigned int enable;        /* Non-zero if this channel should be created. */
    IMPPayloadType payloadType; /* Encoded payload type for this branch. */
    IMPFSChnAttr fs_chn_attr;   /* Frame-source attributes for the channel. */
    IMPCell framesource_chn;    /* Source IMP cell used in the media graph. */
    IMPCell imp_encoder;        /* Encoder IMP cell bound to this source. */
};

/**
 * @brief Runtime sensor configuration passed into sample_system_init().
 *
 * main.c converts the compile-time SENSOR_* macros into this runtime structure
 * before entering the IMP/ISP helper layer. That keeps initialization code
 * cleaner and makes the selected sensor easy to print in logs.
 */
typedef struct sample_sensor_cfg
{
    char sensor_name[16];               /* Sensor driver name expected by tx-isp. */
    int sensor_cubs_type;               /* Control bus type, usually I2C. */
    unsigned int sensor_i2c_addr;       /* 7-bit sensor I2C address. */
    unsigned int sensor_i2c_adapter_id; /* Linux I2C bus index. */
    int sensor_width;                   /* Active diagnostic width. */
    int sensor_height;                  /* Active diagnostic height. */
    int chn0_en;                        /* Main channel enable flag. */
    int chn1_en;                        /* Secondary channel enable flag. */
    int chn2_en;                        /* Third channel enable flag. */
    int chn3_en;                        /* Fourth/ext channel enable flag. */
    int crop_en;                        /* Whether crop mode is enabled. */
} sample_sensor_cfg_t;

/*
 * ESP32-C3 communication-related constants for the new hardware architecture.
 *
 * These definitions describe transport endpoints only. They do not mean the
 * T23 runs the old vendor networking stack; in the new architecture the T23 is
 * mainly a camera source and SPI master.
 */

/* User-space SPI master device exposed by the T23 SSI0 controller. */
#define SPI_C3_DEVICE                "/dev/spidev0.0"

/* Conservative SPI clock used during early bring-up and protocol tests. */
#define SPI_C3_MAX_SPEED             10000000

/* Optional UART console/debug path to the ESP32-C3. */
#define UART_C3_DEVICE               "/dev/ttyS0"

/* Example LED payload length: 100 RGB LEDs * 3 bytes per LED. */
#define C3_COLOR_DATA_LEN            300

#define CHN_NUM ARRAY_SIZE(chn)

/**
 * @brief Initialize sensor, ISP and IMP core services.
 *
 * This is the first mandatory media step of camera bring-up. If it fails, the
 * root cause is usually below application logic, such as sensor probing,
 * tx-isp modules, or libimp/kernel media-stack compatibility.
 *
 * @param sensor_cfg Active sensor configuration prepared at runtime.
 * @return 0 on success, negative value on failure.
 */
int sample_system_init(sample_sensor_cfg_t sensor_cfg);

/**
 * @brief Shut down the IMP system and detach the active sensor cleanly.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_system_exit(void);

/**
 * @brief Start streaming on all enabled frame-source channels.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_framesource_streamon(void);

/**
 * @brief Stop streaming on all enabled frame-source channels.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_framesource_streamoff(void);

/**
 * @brief Create and configure all enabled frame-source channels.
 *
 * This is the main "raw video path is ready" step after ISP core init.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_framesource_init(void);

/**
 * @brief Destroy all previously created frame-source channels.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_framesource_exit(void);

/**
 * @brief Initialize the generic video encoder path.
 *
 * The current rebuild app focuses mainly on JPEG snapshots, but the generic
 * encoder helper is kept for future H.264/H.265 experiments.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_encoder_init(void);

/**
 * @brief Initialize the JPEG encoder channel used by snapshot diagnostics.
 *
 * This is the key boundary between "sensor/framesource work" and
 * "VPU/JPEG encode works". If framesource succeeds but this fails, focus on
 * encoder-side issues rather than sensor-side issues.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_jpeg_init(void);

/**
 * @brief Destroy encoder channels created by sample_encoder_init() or
 * sample_jpeg_init().
 *
 * @return 0 on success, negative value on failure.
 */
int sample_encoder_exit(void);

/**
 * @brief Create OSD regions for a specific encoder group.
 *
 * This helper is not part of the current minimum bring-up path, but is kept so
 * overlays can be re-enabled later without recovering old vendor code.
 *
 * @param grpNum Encoder group number.
 * @return Pointer to allocated region handle array, or NULL on failure.
 */
IMPRgnHandle *sample_osd_init(int grpNum);

/**
 * @brief Destroy OSD regions created by sample_osd_init().
 *
 * @param prHandle Region handle array returned by sample_osd_init().
 * @param grpNum Encoder group number.
 * @return 0 on success, negative value on failure.
 */
int sample_osd_exit(IMPRgnHandle *prHandle, int grpNum);

/**
 * @brief Grab raw frames from enabled channels and save them for inspection.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_get_frame(void);

/**
 * @brief Receive encoded stream data using the vendor sample worker threads.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_get_video_stream(void);

/**
 * @brief Receive encoded streams using file-descriptor polling.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_get_video_stream_byfd(void);

/**
 * @brief Capture JPEG snapshots and save them to disk.
 *
 * In the rebuild project snapshots are intentionally written to /tmp so JPEG
 * testing does not depend on the writable flash partition being mounted.
 *
 * @return 0 on success, negative value on failure.
 */
int sample_get_jpeg_snap(void);

/**
 * @brief Placeholder helper for IR-cut control kept from the vendor sample.
 *
 * @param enable Non-zero to enable, zero to disable.
 * @return 0 on success, negative value on failure.
 */
int sample_SetIRCUT(int enable);

/**
 * @brief Thread routine for software photosensitive-control experiments.
 *
 * @param p Opaque user argument.
 * @return Thread return value.
 */
void *sample_soft_photosensitive_ctrl(void *p);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __CAMERA_COMMON_H__ */
