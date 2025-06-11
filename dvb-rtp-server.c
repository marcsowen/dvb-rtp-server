#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <stdbool.h>

#define TS_CHUNK 1316
#define TCP_PORT 1234
#define DEFAULT_SYMBOL_RATE 6900000

fe_modulation_t parse_modulation(const char *mod_str) {
    if (strcasecmp(mod_str, "QAM16") == 0) return QAM_16;
    if (strcasecmp(mod_str, "QAM32") == 0) return QAM_32;
    if (strcasecmp(mod_str, "QAM64") == 0) return QAM_64;
    if (strcasecmp(mod_str, "QAM128") == 0) return QAM_128;
    if (strcasecmp(mod_str, "QAM256") == 0) return QAM_256;
    fprintf(stderr, "Invalid modulation: %s\n", mod_str);
    exit(1);
}

void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s --freq <MHz> --mod <QAMxx> [--stdout]\n", prog);
    fprintf(stderr, "Example: %s --freq 546 --mod QAM256\n", prog);
    fprintf(stderr, "         %s --freq 546 --mod QAM256 --stdout > stream.ts\n", prog);
}

int tune_dvb(int fe_fd, uint32_t freq_hz, uint32_t sr, fe_modulation_t mod) {
    struct dvb_frontend_parameters params = {
        .frequency = freq_hz,
        .inversion = INVERSION_AUTO,
        .u.qam.symbol_rate = sr,
        .u.qam.fec_inner = FEC_AUTO,
        .u.qam.modulation = mod
    };
    if (ioctl(fe_fd, FE_SET_FRONTEND, &params) < 0) {
        perror("FE_SET_FRONTEND");
        return -1;
    }

    sleep(1);
    fe_status_t status;
    if (ioctl(fe_fd, FE_READ_STATUS, &status) == 0) {
        if (status & FE_HAS_LOCK) {
            fprintf(stderr, "Tuner: Lock ok ✅\n");
        } else {
            fprintf(stderr, "Tuner: No lock ❌ (status 0x%02x)\n", status);
            return -1;
        }
    }
    return 0;
}

int start_demux() {
    int fd = open("/dev/dvb/adapter0/demux0", O_RDWR);
    if (fd < 0) { perror("open demux"); return -1; }

    struct dmx_pes_filter_params f = {
        .pid = 8192,
        .input = DMX_IN_FRONTEND,
        .output = DMX_OUT_TS_TAP,
        .pes_type = DMX_PES_OTHER,
        .flags = DMX_IMMEDIATE_START
    };

    if (ioctl(fd, DMX_SET_PES_FILTER, &f) < 0) {
        perror("DMX_SET_PES_FILTER");
        close(fd);
        return -1;
    }
    return fd;
}

int start_tcp_server(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    listen(s, 1);
    printf("Waiting for TCP connection on port %d...\n", port);
    return s;
}

int main(int argc, char *argv[]) {
    uint32_t freq_mhz = 0;
    fe_modulation_t modulation = QAM_256;
    bool to_stdout = false;

    static struct option long_opts[] = {
        {"freq", required_argument, 0, 'f'},
        {"mod",  required_argument, 0, 'm'},
        {"stdout", no_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:m:s", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'f': freq_mhz = atoi(optarg); break;
            case 'm': modulation = parse_modulation(optarg); break;
            case 's': to_stdout = true; break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (freq_mhz == 0) {
        print_usage(argv[0]);
        return 1;
    }

    uint32_t freq_hz = freq_mhz * 1000000;

    int fe_fd = open("/dev/dvb/adapter0/frontend0", O_RDWR);
    if (fe_fd < 0) { perror("open frontend"); return 1; }

    if (tune_dvb(fe_fd, freq_hz, DEFAULT_SYMBOL_RATE, modulation) < 0)
        return 1;

    int demux_fd = start_demux();
    if (demux_fd < 0) return 1;

    int dvr_fd = open("/dev/dvb/adapter0/dvr0", O_RDONLY | O_NONBLOCK);
    if (dvr_fd < 0) { perror("open dvr"); return 1; }

    int out_fd;
    if (to_stdout) {
        out_fd = STDOUT_FILENO;
        setvbuf(stdout, NULL, _IONBF, 0); // unbuffered output
    } else {
        int server_fd = start_tcp_server(TCP_PORT);
        out_fd = accept(server_fd, NULL, NULL);
        printf("Client connected – streaming...\n");
    }

    uint8_t buf[TS_CHUNK];
    while (1) {
        ssize_t r = read(dvr_fd, buf, sizeof(buf));
        if (r > 0) {
            ssize_t w = write(out_fd, buf, r);
            if (w < 0) break;
        } else {
            usleep(1000);
        }
    }

    return 0;
}

