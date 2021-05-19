/*
 * Copyright (C) 2021 LIG
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     examples
 * @{
 *
 * @file
 * @brief       Envoie commande à l'IdoDoor
 *
 * @author      Germain Lemasson <germain.lemasson@univ-grenoble-alpes.fr>
 *
 * @}
 */

#include <stdio.h>
#include <time.h>
#include <string.h>

#include "thread.h"
#include "shell.h"
#include "shell_commands.h"
#include "crypto/aes.h"

#include "fmt.h"
#include "board.h"

#include "periph/uart.h"
#include "periph/rtc.h"

#include "stdio_uart.h"

#include "net/netdev.h"
#include "net/netdev/lora.h"
#include "net/lora.h"

#include "sx127x_internal.h"
#include "sx127x_params.h"
#include "sx127x_netdev.h"

#include "ido_common.h"
#include "ido_crypto.h"


#define SX127X_LORA_MSG_QUEUE   (16U)
#define SX127X_STACKSIZE        (THREAD_STACKSIZE_DEFAULT)

#define MSG_TYPE_ISR            (0x3456)

static char stack[SX127X_STACKSIZE];
static kernel_pid_t _recv_pid;

static uint8_t message_enc[AES_BLOCK_SIZE];
static sx127x_t sx127x;

cipher_context_t cipher;
ido_msg_t msg={0};

//Function remplaçant stdio_write qui rajoute le \r
//Ceci est indispensable pour avoir un affichage correct sur le minitel
ssize_t __wrap_stdio_write(const void* buffer, size_t len)
{
    const uint8_t *buf = (const uint8_t *)buffer;
    const uint8_t cr = '\r';
    size_t rem = len;
    while (rem--) {
            if (*buf == '\n')
                    uart_write(STDIO_UART_DEV, &cr, 1);
            uart_write(STDIO_UART_DEV, buf, 1);
            buf++;
    }

    return len;
}

static int _print_time(struct tm *time)
{
    printf("%04i-%02i-%02i %02i:%02i:%02i\n",
            time->tm_year + 1900, time->tm_mon + 1, time->tm_mday,
            time->tm_hour, time->tm_min, time->tm_sec
          );
    return 0;
}
static int dow(int year, int month, int day)
{
    /* calculate the day of week using Tøndering's algorithm */
    static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    year -= month < 3;
    return (year + year/4 - year/100 + year/400 + t[month-1] + day) % 7;
}

/* Parse le string "YYYY-MM-DD HH:MM:SS" en structure time
 */
static int _parse_time(char **argv, struct tm *time)
{
    short i;
    char *end;

    i = strtol(argv[0], &end, 10);
    time->tm_year = i - 1900;

    i = strtol(end + 1, &end, 10);
    time->tm_mon = i - 1;

    i = strtol(end + 1, &end, 10);
    time->tm_mday = i;

    i = strtol(argv[1], &end, 10);
    time->tm_hour = i;

    i = strtol(end + 1, &end, 10);
    time->tm_min = i;

    i = strtol(end + 1, &end, 10);
    time->tm_sec = i;

    time->tm_wday = dow(time->tm_year + 1900, time->tm_mon + 1, time->tm_mday);
    time->tm_isdst = -1; /* undefined */

    return 0;
}

/* Envoi la commande avec LORA
 * Crypte le message avant l'envoie
 */
void send_cmd(uint8_t cmd, uint32_t time){
    msg.msg.cmd=cmd;
    msg.msg.time=time;

    printf("cmd:0x%x time:%ld\r\n", cmd, time);

    aes_encrypt(&cipher, msg.aes_plain, message_enc);

    iolist_t iolist = {
        .iol_base = message_enc,
        .iol_len = AES_BLOCK_SIZE
    };

    netdev_t *netdev = (netdev_t *)&sx127x;
    if (netdev->driver->send(netdev, &iolist) == -ENOTSUP) {
        puts("Cannot send: radio is still transmitting\r");
    }



}

/* Shell callback pour le changer l'heure
 */
int settime_cmd(int argc, char **argv)
{
    if (argc <= 1) {
        puts("usage: settime YYYY-MM-DD HH:MM:SS\r");
        return -1;
    }
    struct tm now;
    if (_parse_time(argv+1, &now) == 0) {
        _print_time(&now);
        send_cmd(IDO_CMD_TIME,rtc_mktime(&now));
    }
    
    return 0;
}

/* Shell callback pour le changer définir la date de réouverture
 */
int setopentime_cmd(int argc, char **argv)
{
    if (argc <= 1) {
        puts("usage: setopentime YYYY-MM-DD HH:MM:SS\r");
        return -1;
    }
    struct tm now;
    if (_parse_time(argv+1, &now) == 0) {
        _print_time(&now);
        send_cmd(IDO_CMD_OPENTIME,rtc_mktime(&now));
    }
    

    return 0;
}
/* Shell callback pour afficher les horraires d'ouverture (écran par défaut)
 */
int senddefault_cmd(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    send_cmd(IDO_CMD_DEFAULT,0);

    return 0;
}
/* Shell callback pour mettre la base Idosens en veille
 */
int sendidle_cmd(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    send_cmd(IDO_CMD_IDLE,0);
    return 0;
}
static const shell_command_t shell_commands[] = {
    { "setopentime",    "\tsetopentime YYYY-MM-DD HH:MM:SS\r\n\t\t\tset opening hour\r",     setopentime_cmd },
    { "settime",    "\tsettime YYYY-MM-DD HH:MM:SS\r\n\t\t\tset the current time\r",     settime_cmd },
    { "default",    "send reset screen\r",    senddefault_cmd },
    { "idle",    "send idle\r",    sendidle_cmd },
    { NULL, NULL, NULL }
};

/* Callback des  intéruption de la stack LORA
 */
static void _event_cb(netdev_t *dev, netdev_event_t event)
{
    if (event == NETDEV_EVENT_ISR) {
        msg_t msg;

        msg.type = MSG_TYPE_ISR;
        msg.content.ptr = dev;

        if (msg_send(&msg, _recv_pid) <= 0) {
            puts("gnrc_netdev: possibly lost interrupt.\r");
        }
    }
    else {
        switch (event) {
            case NETDEV_EVENT_TX_COMPLETE:
                sx127x_set_sleep(&sx127x);
                puts("Transmission completed\r");
                break;

            case NETDEV_EVENT_CAD_DONE:
                break;

            case NETDEV_EVENT_TX_TIMEOUT:
                sx127x_set_sleep(&sx127x);
                break;

            default:
                printf("Unexpected netdev event received: %d\r\n", event);
                break;
        }
    }
}

/* Thread qui traite les intéruptions du module lora
 */
void *_recv_thread(void *arg)
{
    (void)arg;

    static msg_t _msg_q[SX127X_LORA_MSG_QUEUE];
    msg_init_queue(_msg_q, SX127X_LORA_MSG_QUEUE);

    while (1) {
        msg_t msg;
        msg_receive(&msg);
        if (msg.type == MSG_TYPE_ISR) {
            netdev_t *dev = msg.content.ptr;
            dev->driver->isr(dev);
        }
        else {
            puts("Unexpected msg type\r");
        }
    }
}

int main(void)
{
    //Paramètre UART du minitel 7 bit, parité pair, stop bit 1
    uart_mode 	( UART_DEV(0),
		UART_DATA_BITS_7,
        UART_PARITY_EVEN,
        UART_STOP_BITS_1
	);

    //initialisation de la crypto
    aes_init(&cipher, IDO_KEY, 16);

    sx127x.params = sx127x_params[0];
    netdev_t *netdev = (netdev_t *)&sx127x;
    netdev->driver = &sx127x_driver;

    if (netdev->driver->init(netdev) < 0) {
        puts("Failed to initialize SX127x device, exiting\r");
        return 1;
    }
    netdev->event_callback = _event_cb;
    _recv_pid = thread_create(stack, sizeof(stack), THREAD_PRIORITY_MAIN - 1,
                              THREAD_CREATE_STACKTEST, _recv_thread, NULL,
                              "recv_thread");
    if (_recv_pid <= KERNEL_PID_UNDEF) {
        puts("Creation of receiver thread failed\r");
        return 1;
    }

    uint8_t lora_bw=125;
    uint8_t lora_sf=7;
    uint8_t lora_cr=5;
    netdev->driver->set(netdev, NETOPT_BANDWIDTH,
                        &lora_bw, sizeof(lora_bw));
    netdev->driver->set(netdev, NETOPT_SPREADING_FACTOR,
                        &lora_sf, sizeof(lora_sf));
    netdev->driver->set(netdev, NETOPT_CODING_RATE,
                        &lora_cr, sizeof(lora_cr));


    puts("Generated RIOT application: 'IdoTel'\r");
    puts("Initialization successful - starting the shell now\r");
    char line_buf[SHELL_DEFAULT_BUFSIZE];
    shell_run(shell_commands, line_buf, SHELL_DEFAULT_BUFSIZE);

    return 0;
}
