// link_layer.c
// RCOM 2026/2027
//
// Link layer protocol implementation

#include "link_layer.h"
#include "serial_port.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FLAG   0x7E
#define ESC    0x7D
#define A_TX   0x03   // Commands from Tx / replies from Rx
#define A_RX   0x01   // Commands from Rx / replies from Tx
#define C_SET  0x03
#define C_UA   0x07
#define C_DISC 0x0B
#define C_RR0  0xAA
#define C_RR1  0xAB
#define C_REJ0 0x54
#define C_REJ1 0x55
#define C_I0   0x00
#define C_I1   0x80

typedef enum { ST_START, ST_FLAG, ST_A, ST_C, ST_BCC, ST_STOP } State;

static volatile int alarmEnabled = 0;
static LinkLayer params;      // Saved at llOpen*, used throughout the session
static int txNs = 0;          // Tx: sequence number of next I frame (0 or 1)
static int rxExpected = 0;    // Rx: sequence number of expected I frame (0 or 1)

static void alarmHandler(int sig)
{
    (void)sig;
    alarmEnabled = 0;
}

static void installAlarm(void)
{
    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;   // No SA_RESTART: read() must be interrupted on timeout
    if (sigaction(SIGALRM, &act, NULL) == -1)
    {
        perror("sigaction");
        exit(1);
    }
}

/**
 * Reads ONE supervision/unnumbered frame (F A C BCC1 F) for a specified address 'a'.
 * Returns the Control byte received, or -1 if the timeout expired (when withTimeout == 1).
 */
static int readSFrame(unsigned char a, int withTimeout)
{
    State state = ST_START;
    unsigned char byte, c = 0;

    while (state != ST_STOP)
    {
        if (withTimeout && !alarmEnabled)
            return -1;

        int res = readByteSerialPort(&byte);
        if (res <= 0)
        {
            if (withTimeout && !alarmEnabled)
                return -1;
            usleep(1000);  // Prevent 100% CPU busy-waiting
            continue;
        }

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
            if (byte == FLAG) state = ST_FLAG;
            else { c = byte; state = ST_C; }
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
    return c;
}

static void sendSFrame(unsigned char a, unsigned char c)
{
    unsigned char f[5] = {FLAG, a, c, a ^ c, FLAG};
    writeBytesSerialPort(f, 5);
}

/**
 * Sends a supervision frame 'c' with address 'a', arming an alarm and waiting
 * for 'expected' reply with address 'ea'. Retries up to nRetransmissions times.
 */
static int sendAndWait(unsigned char a, unsigned char c, unsigned char ea, unsigned char expected)
{
    for (int attempt = 0; attempt < params.nRetransmissions; attempt++)
    {
        sendSFrame(a, c);
        alarmEnabled = 1;
        alarm(params.timeout);
        int r = readSFrame(ea, 1);
        alarm(0);
        if (r == expected) return 0;
    }
    return -1;
}

static int openPort(LinkLayer p)
{
    params = p;
    txNs = 0;
    rxExpected = 0;
    if (openSerialPort(p.serialPort, p.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }
    installAlarm();
    return 0;
}

////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llOpenTx(LinkLayer llParameters)
{
    if (openPort(llParameters) < 0) return -1;
    if (sendAndWait(A_TX, C_SET, A_TX, C_UA) < 0)
    {
        printf("llOpenTx: No UA received after %d attempts. Giving up.\n", params.nRetransmissions);
        closeSerialPort();
        return -1;
    }
    printf("Connection established (SET sent, UA received)\n");
    return 0;
}

int llOpenRx(LinkLayer llParameters)
{
    if (openPort(llParameters) < 0) return -1;
    while (readSFrame(A_TX, 0) != C_SET) ;
    sendSFrame(A_TX, C_UA);
    printf("Connection established (SET received, UA sent)\n");
    return 0;
}

////////////////////////////////////////////////
// LLSEND (Tx side of Stop & Wait)
////////////////////////////////////////////////
int llSend(const unsigned char *buf, int bufSize)
{
    if (bufSize < 0) return -1;

    // Allocate memory: worst case is every payload byte + BCC2 being stuffed (2x) + headers
    unsigned char *frame = malloc(2 * (bufSize + 1) + 6);
    if (!frame) return -1;

    int n = 0;
    frame[n++] = FLAG;
    frame[n++] = A_TX;
    frame[n++] = txNs ? C_I1 : C_I0;
    frame[n++] = frame[1] ^ frame[2];                 // BCC1

    unsigned char bcc2 = 0;
    for (int i = 0; i <= bufSize; i++)                // i == bufSize corresponds to BCC2
    {
        unsigned char b = (i < bufSize) ? buf[i] : bcc2;
        if (i < bufSize) bcc2 ^= b;                   // Compute BCC2 over original bytes
        if (b == FLAG || b == ESC)
        {
            frame[n++] = ESC;
            frame[n++] = b ^ 0x20;
        }
        else
        {
            frame[n++] = b;
        }
    }
    frame[n++] = FLAG;

    int attempts = 0;
    while (attempts < params.nRetransmissions)
    {
        writeBytesSerialPort(frame, n);
        attempts++;

        alarmEnabled = 1;
        alarm(params.timeout);
        int r = readSFrame(A_TX, 1);
        alarm(0);

        unsigned char rrOk = txNs ? C_RR0 : C_RR1;    // Expected acknowledgment RR(Ns+1)
        if (r == rrOk)
        {
            txNs ^= 1;
            free(frame);
            return bufSize;
        }

        // On REJ, duplicate RR, or timeout: retransmit immediately
        printf("llSend: %s, retransmitting (attempt %d/%d)\n",
               r < 0 ? "timeout" : "REJ/bad ACK", attempts, params.nRetransmissions);
    }

    free(frame);
    return -1;
}

////////////////////////////////////////////////
// LLRECEIVE (Rx side of Stop & Wait)
////////////////////////////////////////////////
int llReceive(unsigned char *packet)
{
    unsigned char rr[2]  = {C_RR0, C_RR1};
    unsigned char rej[2] = {C_REJ0, C_REJ1};

    for (;;)
    {
        enum { S_START, S_FLAG, S_A, S_C, S_BCC1, S_DATA } st = S_START;
        unsigned char byte, c = 0;
        unsigned char data[2 * MAX_PAYLOAD_SIZE + 8];
        int len = 0, esc = 0, done = 0;

        while (!done)
        {
            int res = readByteSerialPort(&byte);
            if (res <= 0)
            {
                usleep(1000);
                continue;
            }

            switch (st)
            {
            case S_START:
                if (byte == FLAG) st = S_FLAG;
                break;
            case S_FLAG:
                if (byte == A_TX) st = S_A;
                else if (byte != FLAG) st = S_START;
                break;
            case S_A:
                if (byte == C_I0 || byte == C_I1) { c = byte; st = S_C; }
                else if (byte == C_DISC) { return 0; } // Graceful termination detection
                else if (byte == FLAG) st = S_FLAG;
                else st = S_START;
                break;
            case S_C:
                if (byte == (A_TX ^ c)) { st = S_BCC1; len = 0; esc = 0; }
                else if (byte == FLAG) st = S_FLAG;
                else st = ST_START;
                break;
            case S_BCC1:
            case S_DATA:
                st = S_DATA;
                if (byte == FLAG) { done = 1; break; }
                if (len >= (int)sizeof(data)) { st = S_START; break; }  // Guard against overflow
                if (esc) { data[len++] = byte ^ 0x20; esc = 0; }
                else if (byte == ESC) esc = 1;
                else data[len++] = byte;
                break;
            }
        }

        // Validate received frame
        int ns = (c == C_I1);
        if (len < 1) continue;                         // Empty payload / no BCC2

        unsigned char bcc2 = 0;
        for (int i = 0; i < len - 1; i++) bcc2 ^= data[i];
        int bcc2ok = (bcc2 == data[len - 1]);

        if (ns == rxExpected)
        {
            if (bcc2ok)
            {
                rxExpected ^= 1;
                sendSFrame(A_TX, rr[rxExpected]);      // Positive ACK: RR(Ns+1)
                memcpy(packet, data, len - 1);
                return len - 1;
            }
            // Error in data field of a new frame -> reject to request immediate retransmission
            sendSFrame(A_TX, rej[rxExpected]);
        }
        else
        {
            // Duplicate frame: discard payload, re-send last positive RR
            sendSFrame(A_TX, rr[rxExpected]);
        }
    }
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llCloseTx(void)
{
    int r = sendAndWait(A_TX, C_DISC, A_RX, C_DISC);
    if (r == 0) sendSFrame(A_RX, C_UA);
    sleep(1);                                          // Allow last frame to clear the UART buffer
    closeSerialPort();
    return r;
}

int llCloseRx(void)
{
    while (readSFrame(A_TX, 0) != C_DISC) ;
    // Protect DISC response with retries/timeout waiting for Tx's final UA
    int r = sendAndWait(A_RX, C_DISC, A_RX, C_UA);
    sleep(1);
    closeSerialPort();
    return r;
}