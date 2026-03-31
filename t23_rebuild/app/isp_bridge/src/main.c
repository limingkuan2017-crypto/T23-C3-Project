#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
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
#include "t23_c3_protocol.h"

#define UART_DEFAULT_DEVICE "/dev/ttyS0"
#define UART_DEFAULT_BAUD B115200
#define UART_RX_BUF_SIZE 256
#define JPEG_MAX_SIZE (512 * 1024)

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

static volatile sig_atomic_t g_running = 1;
static unsigned char g_jpeg_buf[JPEG_MAX_SIZE];
static int g_jpeg_chn_id = -1;
static int g_jpeg_recv_started = 0;
static int g_spi_fd = -1;
static int g_data_ready_fd = -1;

static void usage(const char *prog)
{
    printf("Usage: %s [--port /dev/ttyS0] [--baud 115200]\n", prog);
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
