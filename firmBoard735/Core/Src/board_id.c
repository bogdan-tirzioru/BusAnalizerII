#include "board_id.h"

#include <stdio.h>


typedef struct
{
    uint32_t uid0;
    uint32_t uid1;
    uint32_t uid2;

    BoardID_t board_id;
    const char *name;

} BoardID_Entry_t;


/*
 * Known physical boards.
 *
 * We will fill these UID values after reading the two boards.
 */
static const BoardID_Entry_t board_table[] =
{
    {
        0x002B0006,
        0x31395113,
        0x39393533,
        BOARD_ID_BUSANALYZER_H725_01,
        "BusAnalyzerII H725 #1"
    },

    {
        0x0024002B,
        0x32325115,
        0x31343535,
        BOARD_ID_BUSANALYZER_H735_01,
        "BusAnalyzerII H735 #1"
    }
};


static uint32_t board_uid0;
static uint32_t board_uid1;
static uint32_t board_uid2;

static BoardID_t current_board = BOARD_ID_UNKNOWN;
static const char *current_board_name = "UNKNOWN";


void BoardID_Init(void)
{
    board_uid0 = HAL_GetUIDw0();
    board_uid1 = HAL_GetUIDw1();
    board_uid2 = HAL_GetUIDw2();

    current_board = BOARD_ID_UNKNOWN;
    current_board_name = "UNKNOWN";

    const uint32_t number_of_boards =
        sizeof(board_table) / sizeof(board_table[0]);

    for (uint32_t i = 0; i < number_of_boards; i++)
    {
        if ((board_uid0 == board_table[i].uid0) &&
            (board_uid1 == board_table[i].uid1) &&
            (board_uid2 == board_table[i].uid2))
        {
            current_board = board_table[i].board_id;
            current_board_name = board_table[i].name;
            break;
        }
    }
}


BoardID_t BoardID_Get(void)
{
    return current_board;
}


const char *BoardID_GetName(void)
{
    return current_board_name;
}


void BoardID_Print(UART_HandleTypeDef *huart)
{
    (void)huart;

    char buffer[384];

    uint32_t device_id = HAL_GetDEVID();
    uint32_t revision_id = HAL_GetREVID();

    int len = snprintf(
        buffer,
        sizeof(buffer),
        "\r\n"
        "========================================\r\n"
        " BusAnalyzerII board identification\r\n"
        "========================================\r\n"
        " Board      : %s\r\n"
        " UID        : %08lX-%08lX-%08lX\r\n"
        " Device ID  : 0x%03lX\r\n"
        " Revision   : 0x%04lX\r\n"
        "========================================\r\n",
        current_board_name,
        (unsigned long)board_uid0,
        (unsigned long)board_uid1,
        (unsigned long)board_uid2,
        (unsigned long)device_id,
        (unsigned long)revision_id
    );

    if (len > 0)
    {
        if (len >= (int)sizeof(buffer))
        {
            len = sizeof(buffer) - 1;
        }

        buffer[len] = '\0';
        printf("%s", buffer);
    }
}
