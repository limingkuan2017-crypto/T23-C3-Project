#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_MODE_DEFAULT SPI_MODE_0
#define SPI_BITS_DEFAULT 8
#define SPI_SPEED_DEFAULT 10000000U

/*
 * T23-side SPI diagnostic utility.
 *
 * It answers three different questions:
 * - can the Linux SPI device node be opened and configured?
 * - can T23 generate master transfers on the new wiring?
 * - can T23 read the new Data Ready input line from C3?
 *
 * It does not try to implement the final image transport protocol yet.
 */
/*
 * New hardware defines Data Ready as ESP32-C3 -> T23N.
 * On T23 the line should be treated as an input.
 *
 * User mapping:
 *   T23 pin 68 -> Linux GPIO 53
 */
#define DATA_READY_GPIO 53

/**
 * @brief Print command-line usage for the SPI diagnostic tool.
 *
 * @param prog Program name displayed in the help text.
 */
static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s info\n", prog);
    printf("  %s read-dr\n", prog);
    printf("  %s wait-dr <timeout_ms>\n", prog);
    printf("  %s xfer <byte0> [byte1 ...]\n", prog);
    printf("  %s xfer-dr <byte0> [byte1 ...]\n", prog);
    printf("\n");
    printf("Examples:\n");
    printf("  %s info\n", prog);
    printf("  %s xfer a5 5a 00 ff\n", prog);
    printf("  %s wait-dr 3000\n", prog);
    printf("  %s xfer-dr a5 5a 11 22\n", prog);
}

/**
 * @brief Export a GPIO through sysfs if it is not already exported.
 *
 * @param gpio Linux GPIO number.
 *
 * @return 0 on success, -1 on failure.
 */
static int ensure_gpio_exported(int gpio)
{
    char path[64];
    int fd;
    int ret;
    char buf[16];

    /* Sysfs GPIO is enough for low-rate bring-up checks and easy to inspect. */
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

/**
 * @brief Write a short text value into a sysfs file.
 *
 * @param path File path to write.
 * @param value Text value to write.
 *
 * @return 0 on success, -1 on failure.
 */
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

/**
 * @brief Configure the C3 `Data Ready` signal as an input on T23.
 *
 * @return 0 on success, -1 on failure.
 */
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

/**
 * @brief Read the current logic level of the `Data Ready` GPIO.
 *
 * @param value Output pointer that receives 0 or 1.
 *
 * @return 0 on success, -1 on failure.
 */
static int read_data_ready_value(int *value)
{
    char path[128];
    char ch;
    int fd;

    if (setup_data_ready_input() < 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", DATA_READY_GPIO);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    if (read(fd, &ch, 1) != 1) {
        perror("read gpio value");
        close(fd);
        return -1;
    }

    close(fd);
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

/**
 * @brief Read the current monotonic time in milliseconds.
 *
 * @return Current monotonic time in milliseconds.
 */
static long long monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/**
 * @brief Wait until `Data Ready` becomes high or the timeout expires.
 *
 * @param timeout_ms Timeout in milliseconds.
 *
 * @return 0 if the line becomes high, 1 on timeout, -1 on error.
 */
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
            printf("data-ready: high\n");
            return 0;
        }
        /* Poll at 1 ms intervals; simple and sufficient for manual testing. */
        usleep(1000);
    }

    printf("data-ready: timeout after %d ms\n", timeout_ms);
    return 1;
}

/**
 * @brief Open and configure the Linux SPI device used for T23<->C3 testing.
 *
 * @param fd_out Output pointer that receives the opened file descriptor.
 *
 * @return 0 on success, -1 on failure.
 */
static int spi_open_configure(int *fd_out)
{
    int fd;
    uint8_t mode = SPI_MODE_DEFAULT;
    uint8_t bits = SPI_BITS_DEFAULT;
    uint32_t speed = SPI_SPEED_DEFAULT;

    /* Fail early if spidev or pinmux is not ready. */
    fd = open(SPI_DEVICE, O_RDWR);
    if (fd < 0) {
        perror(SPI_DEVICE);
        return -1;
    }

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
        perror("SPI_IOC_WR_MODE");
        close(fd);
        return -1;
    }
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        perror("SPI_IOC_WR_BITS_PER_WORD");
        close(fd);
        return -1;
    }
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("SPI_IOC_WR_MAX_SPEED_HZ");
        close(fd);
        return -1;
    }

    *fd_out = fd;
    return 0;
}

/**
 * @brief Print the currently configured SPI device parameters.
 *
 * @return 0 on success, non-zero on failure.
 */
static int spi_print_info(void)
{
    int fd;
    uint8_t mode;
    uint8_t bits;
    uint32_t speed;

    if (spi_open_configure(&fd) < 0) {
        return 1;
    }

    if (ioctl(fd, SPI_IOC_RD_MODE, &mode) < 0) {
        perror("SPI_IOC_RD_MODE");
        close(fd);
        return 1;
    }
    if (ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &bits) < 0) {
        perror("SPI_IOC_RD_BITS_PER_WORD");
        close(fd);
        return 1;
    }
    if (ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed) < 0) {
        perror("SPI_IOC_RD_MAX_SPEED_HZ");
        close(fd);
        return 1;
    }

    printf("spi-device : %s\n", SPI_DEVICE);
    printf("spi-mode   : %u\n", mode);
    printf("spi-bits   : %u\n", bits);
    printf("spi-speed  : %u\n", speed);
    printf("data-ready : gpio%d input (C3 -> T23)\n", DATA_READY_GPIO);

    close(fd);
    return 0;
}

/**
 * @brief Parse hex byte arguments from the command line into a byte buffer.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param start_idx First index containing byte values.
 * @param buf Output buffer.
 * @param len_out Output pointer that receives parsed length.
 *
 * @return 0 on success, -1 on failure.
 */
static int parse_bytes(int argc, char *argv[], int start_idx, uint8_t *buf, size_t *len_out)
{
    int i;
    char *end;
    unsigned long v;
    size_t out_len = 0;

    for (i = start_idx; i < argc; ++i) {
        if (out_len >= 256) {
            fprintf(stderr, "too many bytes, max 256\n");
            return -1;
        }
        v = strtoul(argv[i], &end, 16);
        if (*argv[i] == '\0' || *end != '\0' || v > 0xffUL) {
            fprintf(stderr, "invalid byte: %s\n", argv[i]);
            return -1;
        }
        buf[out_len++] = (uint8_t)v;
    }

    if (out_len == 0) {
        fprintf(stderr, "need at least one byte\n");
        return -1;
    }

    *len_out = out_len;
    return 0;
}

/**
 * @brief Print a small byte buffer in hexadecimal form.
 *
 * @param prefix Label printed before the data.
 * @param buf Buffer to dump.
 * @param len Number of bytes to print.
 */
static void dump_hex(const char *prefix, const uint8_t *buf, size_t len)
{
    size_t i;

    printf("%s", prefix);
    for (i = 0; i < len; ++i) {
        printf("%s%02x", i == 0 ? "" : " ", buf[i]);
    }
    printf("\n");
}

/**
 * @brief Perform one full-duplex SPI transfer.
 *
 * @param tx Transmit buffer.
 * @param len Number of bytes to transfer.
 *
 * @return 0 on success, non-zero on failure.
 */
static int spi_transfer_bytes(const uint8_t *tx, size_t len)
{
    int fd;
    int ret;
    uint8_t rx[256];
    uint32_t speed = SPI_SPEED_DEFAULT;
    uint8_t bits = SPI_BITS_DEFAULT;
    struct spi_ioc_transfer tr;

    if (len > sizeof(rx)) {
        fprintf(stderr, "transfer too large\n");
        return 1;
    }

    if (spi_open_configure(&fd) < 0) {
        return 1;
    }

    memset(rx, 0, sizeof(rx));
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = (uint32_t)len;
    tr.speed_hz = speed;
    tr.bits_per_word = bits;
    tr.cs_change = 0;

    /* Full-duplex SPI: transmit and receive occur during the same clock burst. */
    ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < 0) {
        perror("SPI_IOC_MESSAGE");
        close(fd);
        return 1;
    }

    dump_hex("tx: ", tx, len);
    dump_hex("rx: ", rx, len);
    printf("transfer-bytes: %d\n", ret);

    close(fd);
    return 0;
}

/**
 * @brief Entry point for the T23 SPI diagnostic tool.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 *
 * @return 0 on success, non-zero on failure.
 */
int main(int argc, char *argv[])
{
    uint8_t tx[256];
    size_t len;
    int timeout_ms;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "info") == 0) {
        return spi_print_info();
    }

    if (strcmp(argv[1], "read-dr") == 0) {
        int value;
        if (read_data_ready_value(&value) < 0) {
            return 1;
        }
        printf("data-ready: %d\n", value);
        return 0;
    }

    if (strcmp(argv[1], "wait-dr") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 2;
        }
        timeout_ms = atoi(argv[2]);
        if (timeout_ms <= 0) {
            fprintf(stderr, "invalid timeout: %s\n", argv[2]);
            return 2;
        }
        return wait_data_ready_high(timeout_ms);
    }

    if (strcmp(argv[1], "xfer") == 0) {
        if (parse_bytes(argc, argv, 2, tx, &len) < 0) {
            return 2;
        }
        return spi_transfer_bytes(tx, len);
    }

    if (strcmp(argv[1], "xfer-dr") == 0) {
        if (parse_bytes(argc, argv, 2, tx, &len) < 0) {
            return 2;
        }
        if (wait_data_ready_high(3000) != 0) {
            return 1;
        }
        return spi_transfer_bytes(tx, len);
    }

    usage(argv[0]);
    return 2;
}
