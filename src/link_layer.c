// RCOM 2026/2027
//
// Link layer protocol implementation

#include "link_layer.h"
#include "serial_port.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define FLAG   0x7E
#define A_TX   0x03   // frames from Tx / answers from Rx
#define C_SET  0x03
#define C_UA   0x07

typedef enum { ST_START, ST_FLAG, ST_A, ST_C, ST_BCC, ST_STOP } State;

static volatile int alarmEnabled = 0;
static volatile int alarmCount = 0;

static void alarmHandler(int sig)
{
    alarmEnabled = 0;
    alarmCount++;
    printf("Alarm #%d received\n", alarmCount);
}

// withTimeout = 1 (Tx): returns 0 if the alarm fires before a frame arrives
// withTimeout = 0 (Rx): blocks until a valid frame arrives
static int waitFrame(unsigned char a, unsigned char c, int withTimeout)
{
    State state = ST_START;
    unsigned char byte;

    while (state != ST_STOP)
    {
        if (withTimeout && !alarmEnabled)
            return 0;

        if (readByteSerialPort(&byte) <= 0)
            continue;

        switch (state)
        {
        case ST_START:
            if (byte == FLAG) state = ST_FLAG;
            break;
        case ST_FLAG:
            if (byte == a) state = ST_A;
            else if (byte != FLAG) state = ST_START;
            break;
        case ST_A:
            if (byte == c) state = ST_C;
            else if (byte == FLAG) state = ST_FLAG;
            else state = ST_START;
            break;
        case ST_C:
            if (byte == (a ^ c)) state = ST_BCC;
            else if (byte == FLAG) state = ST_FLAG;
            else state = ST_START;
            break;
        case ST_BCC:
            if (byte == FLAG) state = ST_STOP;
            else state = ST_START;
            break;
        default:
            break;
        }
    }
    return 1;
}

int llOpenTx(LinkLayer llParameters)
{
    if (openSerialPort(llParameters.serialPort, llParameters.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }
    printf("Serial port %s opened\n", llParameters.serialPort);

    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1)
    {
        perror("sigaction");
        exit(1);
    }

    unsigned char set[5] = {FLAG, A_TX, C_SET, A_TX ^ C_SET, FLAG};
    int connected = 0;
    alarmCount = 0;

    while (alarmCount < llParameters.nRetransmissions && !connected)
    {
        writeBytesSerialPort(set, 5);
        printf("SET sent (attempt %d)\n", alarmCount + 1);

        alarmEnabled = 1;
        alarm(llParameters.timeout);

        if (waitFrame(A_TX, C_UA, 1))
        {
            alarm(0);   // disable the pending alarm
            connected = 1;
        }
    }

    if (!connected)
    {
        printf("No UA received after %d attempts. Giving up.\n", alarmCount);
        closeSerialPort();
        return -1;
    }

    printf("Received valid UA frame. Connection established!\n");

    sleep(1);
    if (closeSerialPort() < 0)
    {
        perror("closeSerialPort");
        return -1;
    }
    return 0;
}

int llOpenRx(LinkLayer llParameters)
{
    if (openSerialPort(llParameters.serialPort, llParameters.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }
    printf("Serial port %s opened\n", llParameters.serialPort);

    waitFrame(A_TX, C_SET, 0);
    printf("Received valid SET frame\n");

    unsigned char ua[5] = {FLAG, A_TX, C_UA, A_TX ^ C_UA, FLAG};
    int bytes = writeBytesSerialPort(ua, 5);
    for (int i = 0; i < 5; i++)
        printf("var = 0x%02X\n", ua[i]);
    printf("UA sent (%d bytes)\n", bytes);

    sleep(1);
    if (closeSerialPort() < 0)
    {
        perror("closeSerialPort");
        return -1;
    }
    return 0;
}

////////////////////////////////////////////////
// LLSEND
////////////////////////////////////////////////
int llSend(const unsigned char *buf, int bufSize)
{
    // TODO: Implement this function

    return 0;
}

////////////////////////////////////////////////
// LLRECEIVE
////////////////////////////////////////////////
int llReceive(unsigned char *packet)
{
    // TODO: Implement this function

    return 0;
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llCloseTx()
{
    // TODO: Implement this function

    return 0;
}

int llCloseRx()
{
    // TODO: Implement this function

    return 0;
}
