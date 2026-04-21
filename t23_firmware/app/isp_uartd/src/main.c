/**
 * @file main.c
 * @brief T23 串口 ISP 调参守护进程。
 *
 * 这个程序的职责可以概括成 4 步：
 * 1. 初始化 sensor / ISP / framesource / JPEG 编码通路
 * 2. 打开指定 UART，并切到原始串口模式
 * 3. 按行读取 PC 端命令，例如 GET / SET / SNAP
 * 4. 把参数值或 JPEG 数据通过同一条串口回传给网页端
 *
 * 为什么它是一个“守护进程”而不是一次性小工具：
 * - ISP 调参需要在一条持续存在的图像通路上反复改参数、抓图、再改参数
 * - 如果每次命令都重新初始化 ISP，速度会非常慢，体验也会很差
 * - 所以这里采用“长时间保持 pipeline 存活”的方案
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_encoder.h>

#include "camera_common.h"

#define UART_DEFAULT_DEVICE "/dev/ttyS1"
#define UART_DEFAULT_BAUD B921600
#define UART_RX_BUF_SIZE 256
#define JPEG_MAX_SIZE (512 * 1024)

extern struct chn_conf chn[];

typedef struct {
    /* 串口设备节点，例如 /dev/ttyS1 */
    const char *serial_device;
    /* termios 波特率常量，例如 B115200 / B921600 */
    speed_t baud_rate;
} server_cfg_t;

typedef struct {
    /* 协议里暴露给 PC 端的参数名 */
    const char *name;
    /* 读取当前 ISP 参数值的函数 */
    int (*get_value)(int *value);
    /* 写入新 ISP 参数值的函数 */
    int (*set_value)(int value);
    /* 允许的最小值，用于协议层限幅 */
    int min_value;
    /* 允许的最大值，用于协议层限幅 */
    int max_value;
} isp_param_desc_t;

static volatile sig_atomic_t g_running = 1;
static unsigned char g_jpeg_buf[JPEG_MAX_SIZE];

/**
 * @brief Print command-line usage.
 *
 * @param prog Program name.
 */
static void usage(const char *prog)
{
    printf("Usage: %s [--port /dev/ttyS1] [--baud 921600]\n", prog);
}

/**
 * @brief Handle process-termination signals.
 *
 * @param sig Signal number.
 */
static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/**
 * @brief Build the runtime sensor configuration used by this daemon.
 *
 * @return Sensor configuration structure.
 */
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

/**
 * @brief Create encoder groups for all enabled channels.
 *
 * @return 0 on success, -1 on failure.
 */
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

/**
 * @brief Destroy previously created encoder groups.
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
 * @brief Bind frame-source channels to JPEG encoder channels.
 *
 * @return 0 on success, -1 on failure.
 */
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

/**
 * @brief Unbind frame-source channels from encoder channels.
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
 * @brief Map an integer baud rate to a POSIX termios constant.
 *
 * @param baud Baud rate value from the command line.
 * @return Matching termios speed constant.
 */
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

/**
 * @brief Parse command-line options.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param cfg Output configuration structure.
 *
 * @return 0 on success, -1 on failure.
 */
static int parse_args(int argc, char *argv[], server_cfg_t *cfg)
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

/**
 * @brief Open and configure a serial port for raw line-based command I/O.
 *
 * @param device Serial device path.
 * @param baud_rate termios baud-rate constant.
 *
 * @return File descriptor on success, -1 on failure.
 */
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

/**
 * @brief Send one text line back to the PC.
 *
 * @param fd Serial file descriptor.
 * @param line Text line without trailing newline requirement.
 */
static void send_line(int fd, const char *line)
{
    write(fd, line, strlen(line));
    write(fd, "\n", 1);
}

/**
 * @brief Send formatted text response over the serial link.
 *
 * @param fd Serial file descriptor.
 * @param fmt printf-style format string.
 */
static void sendf(int fd, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send_line(fd, buf);
}

/**
 * @brief Read one complete command line from the serial link.
 *
 * @param fd Serial file descriptor.
 * @param buf Destination buffer.
 * @param buf_size Buffer size in bytes.
 *
 * @return Number of bytes read on success, 0 on EOF, -1 on error.
 */
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
    return (int)len;
}

/**
 * @brief 读取当前亮度。
 *
 * @param value 输出参数，返回当前亮度值。
 * @return 0 表示成功，负值表示底层 IMP 接口调用失败。
 */
static int get_brightness_value(int *value)
{
    unsigned char v = 0;
    int ret = IMP_ISP_Tuning_GetBrightness(&v);
    *value = (int)v;
    return ret;
}

/**
 * @brief 设置亮度。
 *
 * @param value 目标亮度值。
 * @return 0 表示成功，负值表示失败。
 */
static int set_brightness_value(int value)
{
    return IMP_ISP_Tuning_SetBrightness((unsigned char)value);
}

/**
 * @brief 读取当前对比度。
 *
 * @param value 输出参数，返回当前对比度值。
 * @return 0 表示成功，负值表示失败。
 */
static int get_contrast_value(int *value)
{
    unsigned char v = 0;
    int ret = IMP_ISP_Tuning_GetContrast(&v);
    *value = (int)v;
    return ret;
}

/**
 * @brief 设置对比度。
 *
 * @param value 目标对比度值。
 * @return 0 表示成功，负值表示失败。
 */
static int set_contrast_value(int value)
{
    return IMP_ISP_Tuning_SetContrast((unsigned char)value);
}

/**
 * @brief 读取当前锐度。
 *
 * @param value 输出参数，返回当前锐度值。
 * @return 0 表示成功，负值表示失败。
 */
static int get_sharpness_value(int *value)
{
    unsigned char v = 0;
    int ret = IMP_ISP_Tuning_GetSharpness(&v);
    *value = (int)v;
    return ret;
}

/**
 * @brief 设置锐度。
 *
 * @param value 目标锐度值。
 * @return 0 表示成功，负值表示失败。
 */
static int set_sharpness_value(int value)
{
    return IMP_ISP_Tuning_SetSharpness((unsigned char)value);
}

/**
 * @brief 读取当前饱和度。
 *
 * @param value 输出参数，返回当前饱和度值。
 * @return 0 表示成功，负值表示失败。
 */
static int get_saturation_value(int *value)
{
    unsigned char v = 0;
    int ret = IMP_ISP_Tuning_GetSaturation(&v);
    *value = (int)v;
    return ret;
}

/**
 * @brief 设置饱和度。
 *
 * @param value 目标饱和度值。
 * @return 0 表示成功，负值表示失败。
 */
static int set_saturation_value(int value)
{
    return IMP_ISP_Tuning_SetSaturation((unsigned char)value);
}

/**
 * @brief 读取 AE 补偿值。
 *
 * @param value 输出参数，返回当前 AE 补偿值。
 * @return 0 表示成功，负值表示失败。
 */
static int get_ae_comp_value(int *value)
{
    return IMP_ISP_Tuning_GetAeComp(value);
}

/**
 * @brief 设置 AE 补偿值。
 *
 * @param value 目标 AE 补偿值。
 * @return 0 表示成功，负值表示失败。
 */
static int set_ae_comp_value(int value)
{
    return IMP_ISP_Tuning_SetAeComp(value);
}

/**
 * @brief 读取坏点校正强度。
 *
 * @param value 输出参数，返回当前 DPC 强度。
 * @return 0 表示成功，负值表示失败。
 */
static int get_dpc_value(int *value)
{
    unsigned int v = 0;
    int ret = IMP_ISP_Tuning_GetDPC_Strength(&v);
    *value = (int)v;
    return ret;
}

/**
 * @brief 设置坏点校正强度。
 *
 * @param value 目标 DPC 强度值。
 * @return 0 表示成功，负值表示失败。
 */
static int set_dpc_value(int value)
{
    return IMP_ISP_Tuning_SetDPC_Strength((unsigned int)value);
}

/**
 * @brief 读取动态范围压缩强度。
 *
 * @param value 输出参数，返回当前 DRC 强度。
 * @return 0 表示成功，负值表示失败。
 */
static int get_drc_value(int *value)
{
    unsigned int v = 0;
    int ret = IMP_ISP_Tuning_GetDRC_Strength(&v);
    *value = (int)v;
    return ret;
}

/**
 * @brief 设置动态范围压缩强度。
 *
 * @param value 目标 DRC 强度值。
 * @return 0 表示成功，负值表示失败。
 */
static int set_drc_value(int value)
{
    return IMP_ISP_Tuning_SetDRC_Strength((unsigned int)value);
}

/**
 * @brief 读取当前 AWB 色温。
 *
 * @param value 输出参数，返回当前色温值。
 * @return 0 表示成功，负值表示失败。
 */
static int get_awb_ct_value(int *value)
{
    unsigned int ct = 0;
    int ret = IMP_ISP_Tuning_GetAWBCt(&ct);
    *value = (int)ct;
    return ret;
}

/**
 * @brief 设置 AWB 色温。
 *
 * @param value 目标色温值。
 * @return 0 表示成功，负值表示失败。
 */
static int set_awb_ct_value(int value)
{
    unsigned int ct = (unsigned int)value;
    return IMP_ISP_Tuning_SetAwbCt(&ct);
}

/*
 * g_params 把“协议字符串”和“具体的 ISP API”绑定在一起。
 *
 * 例如网页发：
 *   SET BRIGHTNESS 140
 *
 * 本程序会：
 *   1. 在 g_params 中找到 name == "BRIGHTNESS" 的项
 *   2. 调用对应的 set_value()
 *   3. 再调用 get_value() 回读一次，确认实际生效值
 *
 * 这里列出来的并不是“君正公开 SDK 的全部 ISP 参数”，而只是
 * 当前网页 MVP 先接入的 8 个常用参数。选择它们的原则是：
 * - SDK 里已经有稳定的 get/set 接口
 * - 调整效果比较直观，适合用滑块快速验证
 * - 对当前“电视画面采集/氛围灯”场景有第一批调参价值
 *
 * 每一项的大致视觉意义：
 * - BRIGHTNESS: 整体明暗偏移
 * - CONTRAST: 明暗分离度
 * - SHARPNESS: 边缘增强强度
 * - SATURATION: 颜色浓度
 * - AE_COMP: 自动曝光目标亮度
 * - DPC: 坏点校正强度
 * - DRC: 动态范围压缩强度
 * - AWB_CT: 自动白平衡色温目标
 */
static isp_param_desc_t g_params[] = {
    { "BRIGHTNESS", get_brightness_value, set_brightness_value, 0, 255 },   /* 默认 128 */
    { "CONTRAST", get_contrast_value, set_contrast_value, 0, 255 },         /* 默认 128 */
    { "SHARPNESS", get_sharpness_value, set_sharpness_value, 0, 255 },      /* 默认 128 */
    { "SATURATION", get_saturation_value, set_saturation_value, 0, 255 },   /* 默认 128 */
    { "AE_COMP", get_ae_comp_value, set_ae_comp_value, 90, 250 },           /* 自动曝光补偿 */
    { "DPC", get_dpc_value, set_dpc_value, 0, 255 },                        /* 坏点校正强度 */
    { "DRC", get_drc_value, set_drc_value, 0, 255 },                        /* 动态范围压缩 */
    { "AWB_CT", get_awb_ct_value, set_awb_ct_value, 1500, 12000 },          /* 色温，单位 K */
};

/**
 * @brief Find a parameter descriptor by name.
 *
 * @param name Parameter name in uppercase.
 *
 * @return Matching descriptor or NULL if not found.
 */
static isp_param_desc_t *find_param(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); i++) {
        if (strcmp(g_params[i].name, name) == 0) {
            return &g_params[i];
        }
    }

    return NULL;
}

/**
 * @brief Convert a command token to uppercase in place.
 *
 * @param s Mutable string.
 */
static void strtoupper(char *s)
{
    while (*s != '\0') {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
        s++;
    }
}

/**
 * @brief Send all current parameter values to the client.
 *
 * @param fd Serial file descriptor.
 */
static void send_all_values(int fd)
{
    size_t i;

    /* 逐个读取当前参数，并用一行一参数的方式回传给网页端。 */
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

/**
 * @brief Capture one JPEG frame from the active encoder path.
 *
 * @param out_buf Destination buffer.
 * @param out_len Output byte length.
 *
 * @return 0 on success, -1 on failure.
 */
static int capture_jpeg_once(unsigned char *out_buf, size_t *out_len)
{
    int ret;
    unsigned int i;
    int chn_id = -1;
    size_t total = 0;

    /*
     * 当前工程只需要取第一个启用的 framesource 通道做抓拍。
     * JPEG 编码通道编号在 camera_common 体系里是 3 + FS channel index。
     */
    for (i = 0; i < FS_CHN_NUM; i++) {
        if (chn[i].enable) {
            chn_id = 3 + chn[i].index;
            break;
        }
    }
    if (chn_id < 0) {
        return -1;
    }

    ret = IMP_Encoder_StartRecvPic(chn_id);
    if (ret < 0) {
        return -1;
    }

    ret = IMP_Encoder_PollingStream(chn_id, 1000);
    if (ret < 0) {
        IMP_Encoder_StopRecvPic(chn_id);
        return -1;
    }

    {
        IMPEncoderStream stream;

        ret = IMP_Encoder_GetStream(chn_id, &stream, 1);
        if (ret < 0) {
            IMP_Encoder_StopRecvPic(chn_id);
            return -1;
        }

        /*
         * 一帧 JPEG 可能被底层拆成多个 pack，这里把它们拼成一个连续缓冲区，
         * 后面才能通过串口一次性发给 PC 端。
         */
        for (i = 0; i < stream.packCount; i++) {
            if (total + stream.pack[i].length > JPEG_MAX_SIZE) {
                IMP_Encoder_ReleaseStream(chn_id, &stream);
                IMP_Encoder_StopRecvPic(chn_id);
                return -1;
            }
            memcpy(out_buf + total, (void *)stream.pack[i].virAddr, stream.pack[i].length);
            total += stream.pack[i].length;
        }

        IMP_Encoder_ReleaseStream(chn_id, &stream);
    }

    IMP_Encoder_StopRecvPic(chn_id);
    *out_len = total;
    return 0;
}

/**
 * @brief Send one JPEG frame to the client over the serial link.
 *
 * @param fd Serial file descriptor.
 */
static void handle_snap(int fd)
{
    size_t jpeg_len = 0;

    /*
     * 协议约定:
     *   1. 先发一行文本头: JPEG <length>
     *   2. 再紧跟原始 JPEG 二进制数据
     *
     * 前端网页会先解析文本头，知道后面要收多少字节，再切到二进制接收模式。
     */
    if (capture_jpeg_once(g_jpeg_buf, &jpeg_len) < 0) {
        send_line(fd, "ERR SNAP");
        return;
    }

    sendf(fd, "JPEG %u", (unsigned int)jpeg_len);
    write(fd, g_jpeg_buf, jpeg_len);
}

/**
 * @brief Process one text command received from the serial client.
 *
 * @param fd Serial file descriptor.
 * @param line Mutable command line buffer.
 */
static void process_command(int fd, char *line)
{
    char *cmd;
    char *arg1;
    char *arg2;

    cmd = strtok(line, " ");
    if (cmd == NULL) {
        return;
    }
    strtoupper(cmd);

    if (strcmp(cmd, "PING") == 0) {
        send_line(fd, "PONG");
        return;
    }

    if (strcmp(cmd, "HELP") == 0) {
        send_line(fd, "INFO commands: PING, HELP, GET <PARAM|ALL>, SET <PARAM> <VALUE>, SNAP");
        return;
    }

    if (strcmp(cmd, "SNAP") == 0) {
        handle_snap(fd);
        return;
    }

    arg1 = strtok(NULL, " ");
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
            isp_param_desc_t *param = find_param(arg1);
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
        isp_param_desc_t *param = find_param(arg1);

        arg2 = strtok(NULL, " ");
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

        /*
         * SET 成功后再回读一次。
         * 这样网页拿到的是“最终实际生效值”，而不是用户刚输入的原始值。
         * 某些 ISP 参数可能会被底层约束、四舍五入或限幅。
         */
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

/**
 * @brief Initialize the long-running camera pipeline used for tuning.
 *
 * @return 0 on success, -1 on failure.
 */
static int startup_pipeline(void)
{
    sample_sensor_cfg_t sensor_cfg = make_sensor_cfg();

    /*
     * 这里的初始化顺序对应了“持续调参”所需的最小完整 pipeline：
     *   system -> framesource -> encoder group -> jpeg encoder -> bind -> stream on
     *
     * 只要这条链保持活着，网页端就能随时：
     *   - 读/写 ISP 参数
     *   - 拉一张新的 JPEG 快照
     */
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

    return 0;
}

/**
 * @brief Cleanly stop the long-running camera pipeline.
 */
static void shutdown_pipeline(void)
{
    /* 退出顺序基本与 startup_pipeline() 反向对应，避免资源泄漏。 */
    sample_framesource_streamoff();
    unbind_jpeg_channels();
    sample_encoder_exit();
    destroy_groups();
    sample_framesource_exit();
    sample_system_exit();
}

/**
 * @brief Main entry point for the serial ISP tuning daemon.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 *
 * @return 0 on success, non-zero on failure.
 */
int main(int argc, char *argv[])
{
    server_cfg_t cfg;
    int serial_fd;
    char line[UART_RX_BUF_SIZE];

    if (parse_args(argc, argv, &cfg) < 0) {
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("t23_isp_uartd start\n");
    printf("serial device: %s\n", cfg.serial_device);

    if (startup_pipeline() < 0) {
        fprintf(stderr, "failed to start ISP pipeline\n");
        return 1;
    }

    serial_fd = open_serial_port(cfg.serial_device, cfg.baud_rate);
    if (serial_fd < 0) {
        shutdown_pipeline();
        return 1;
    }

    /*
     * READY 是网页端判断“板端守护进程已经真正上线”的第一条可见标志。
     * 只有看到这条，才能说明：
     *   1. ISP pipeline 已经成功初始化
     *   2. 串口也已经打开成功
     */
    send_line(serial_fd, "READY T23_ISP_UARTD 1");

    while (g_running) {
        int ret = read_line(serial_fd, line, sizeof(line));
        if (ret == 0) {
            usleep(10 * 1000);
            continue;
        }
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read_line");
            break;
        }
        if (line[0] == '\0') {
            continue;
        }

        /* 每次循环处理一条完整命令，避免协议状态变复杂。 */
        process_command(serial_fd, line);
    }

    close(serial_fd);
    shutdown_pipeline();
    return 0;
}
