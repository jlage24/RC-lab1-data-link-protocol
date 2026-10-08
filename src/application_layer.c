// RCOM 2026/2027
//
// Application layer protocol implementation

#include "application_layer.h"
#include "link_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Control field values for application packets
#define CTRL_START 0x01
#define CTRL_DATA  0x02
#define CTRL_END   0x03

// TLV parameter types for control packets
#define TYPE_FILE_SIZE 0x00
#define TYPE_FILE_NAME 0x01

// Maximum payload size per data packet (accounting for C, L2, L1 headers)
#define MAX_CHUNK_SIZE (MAX_PAYLOAD_SIZE - 4)

/**
 * Builds a START or END control packet using TLV (Type, Length, Value) format.
 *
 * @param type Control type (CTRL_START or CTRL_END)
 * @param filename Name of the file being transferred
 * @param fileSize Total file size in bytes
 * @param packet Output buffer to store the constructed control packet
 * @return Total number of bytes written to the packet buffer
 */
static int buildControlPacket(unsigned char type, const char *filename, long fileSize, unsigned char *packet)
{
    int idx = 0;
    packet[idx++] = type;

    // TLV 1: File Size
    packet[idx++] = TYPE_FILE_SIZE;
    unsigned char sizeBytes[8];
    int sizeLen = 0;
    long temp = fileSize;
    while (temp > 0) {
        sizeBytes[sizeLen++] = temp & 0xFF;
        temp >>= 8;
    }
    if (sizeLen == 0) sizeBytes[sizeLen++] = 0;
    packet[idx++] = sizeLen;
    for (int i = sizeLen - 1; i >= 0; i--) {
        packet[idx++] = sizeBytes[i];
    }

    // TLV 2: File Name
    packet[idx++] = TYPE_FILE_NAME;
    int nameLen = strlen(filename);
    packet[idx++] = nameLen;
    memcpy(&packet[idx], filename, nameLen);
    idx += nameLen;

    return idx;
}

/**
 * Handles reading the file, chunking it into DATA packets, and sending
 * them sequentially across the link layer protocol.
 *
 * @param filename Name/path of the file to transmit
 */
static void sendFile(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("Error opening input file");
        return;
    }

    // Determine total file size
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // 1. Send START control packet
    unsigned char ctrlPacket[MAX_PAYLOAD_SIZE];
    int ctrlLen = buildControlPacket(CTRL_START, filename, fileSize, ctrlPacket);
    if (llSend(ctrlPacket, ctrlLen) < 0) {
        printf("Failed to send START control packet\n");
        fclose(f);
        return;
    }

    // 2. Read file fragments and transmit DATA packets
    unsigned char fileBuf[MAX_CHUNK_SIZE];
    unsigned char dataPacket[MAX_PAYLOAD_SIZE];
    int bytesRead = 0;
    long totalSent = 0;

    while ((bytesRead = fread(fileBuf, 1, sizeof(fileBuf), f)) > 0)
    {
        dataPacket[0] = CTRL_DATA;
        dataPacket[1] = (bytesRead >> 8) & 0xFF;   // L2 (high byte)
        dataPacket[2] = bytesRead & 0xFF;          // L1 (low byte)
        memcpy(&dataPacket[3], fileBuf, bytesRead);

        if (llSend(dataPacket, bytesRead + 3) < 0) {
            printf("\nFailed to send DATA packet\n");
            fclose(f);
            return;
        }
        totalSent += bytesRead;
        printf("\rProgress: %ld / %ld bytes", totalSent, fileSize);
        fflush(stdout);
    }
    printf("\n");

    // 3. Send END control packet
    ctrlLen = buildControlPacket(CTRL_END, filename, fileSize, ctrlPacket);
    if (llSend(ctrlPacket, ctrlLen) < 0) {
        printf("Failed to send END control packet\n");
    }

    fclose(f);
}

/**
 * Handles receiving packets from the link layer, reconstructing the
 * original file from incoming DATA packets, and terminating on END.
 *
 * @param outputFilename Name/path of the file to write
 */
static void receiveFile(const char *outputFilename)
{
    FILE *f = NULL;
    unsigned char packet[MAX_PAYLOAD_SIZE];
    long expectedSize = 0;
    long receivedBytes = 0;

    while (1)
    {
        int pLen = llReceive(packet);
        if (pLen <= 0) break;

        unsigned char ctrl = packet[0];

        // Process START packet
        if (ctrl == CTRL_START)
        {
            int idx = 1;
            while (idx < pLen) {
                unsigned char type = packet[idx++];
                unsigned char len = packet[idx++];
                if (type == TYPE_FILE_SIZE) {
                    expectedSize = 0;
                    for (int i = 0; i < len; i++) {
                        expectedSize = (expectedSize << 8) | packet[idx + i];
                    }
                }
                idx += len;
            }
            f = fopen(outputFilename, "wb");
            if (!f) {
                perror("Error creating output file");
                return;
            }
            printf("Receiving file. Expected size: %ld bytes\n", expectedSize);
        }
        // Process DATA packet
        else if (ctrl == CTRL_DATA)
        {
            if (!f) continue;
            int dataSize = (packet[1] << 8) | packet[2];
            fwrite(&packet[3], 1, dataSize, f);
            receivedBytes += dataSize;
            printf("\rReceived: %ld / %ld bytes", receivedBytes, expectedSize);
            fflush(stdout);
        }
        // Process END packet
        else if (ctrl == CTRL_END)
        {
            printf("\nEND packet received successfully.\n");
            break;
        }
    }

    if (f) fclose(f);
}

void applicationLayer(const char *serialPort, const char *role, int baudRate,
                      int nTries, int timeout, const char *filename)
{
    LinkLayer llParameters = {
        .baudRate = baudRate,
        .nRetransmissions = nTries,
        .timeout = timeout,
    };
    strncpy(llParameters.serialPort, serialPort, sizeof(llParameters.serialPort) - 1);

    if (strcmp(role, "tx") == 0)
    {
        if (llOpenTx(llParameters) < 0) return;
        sendFile(filename);
        llCloseTx();
    }
    else if (strcmp(role, "rx") == 0)
    {
        if (llOpenRx(llParameters) < 0) return;
        receiveFile(filename);
        llCloseRx();
    }
    else
    {
        printf("Invalid role: %s. Must be 'tx' or 'rx'.\n", role);
    }
}