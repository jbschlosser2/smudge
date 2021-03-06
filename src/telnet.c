/*
 * Copyright (C) 2014 Joel Schlosser All Rights Reserved.
 *
 * This file is part of smudge.
 *
 * smudge is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * smudge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with smudge.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>
#include "telnet.h"

telnet *telnet_create(void)
{
    telnet *tel = malloc(sizeof(telnet));
    tel->cmd_len = 0;
    tel->cmd_ready = 0;

    return tel;
}

void telnet_destroy(telnet *tel)
{
    if(!tel) return;

    free(tel);
}

void telnet_clear_cmd(telnet *tel)
{
    if(!tel) return;

    tel->cmd_ready = 0;
    tel->cmd_len = 0;
}

int telnet_update(telnet *tel, unsigned char byte)
{
    if(!tel) return 0;

    if(tel->cmd_ready) {
        // Don't do anything until the command is handled!
        return 0;
    }

    // The current length of the command determines how the byte
    // affects the state.
    if(tel->cmd_len == 0) {
        // No command starts until an IAC is seen.
        if(byte != TELNET_IAC) return 0;
        tel->cmd[tel->cmd_len++] = byte;
    } else if(tel->cmd_len == 1) {
        tel->cmd[tel->cmd_len++] = byte;
        switch(byte)
        {
            // Two-byte commands.
            case TELNET_IAC:
            case TELNET_NOP:
            case TELNET_DATA_MARK:
            case TELNET_BREAK:
            case TELNET_IP:
            case TELNET_AO:
            case TELNET_AYT:
            case TELNET_EC:
            case TELNET_EL:
            case TELNET_GA:
                tel->cmd_ready = 1;
                break;
            // Three-byte commands.
            case TELNET_WILL:
            case TELNET_WONT:
            case TELNET_DO:
            case TELNET_DONT:
            // >3 byte command.
            case TELNET_SB:
                // Not quite ready yet... need more bytes.
                break;
            default:
                // An unknown command... ignore it and start over.
                telnet_clear_cmd(tel);
                break;
        }
    } else if(tel->cmd_len == 2) {
        unsigned char prev_byte = tel->cmd[tel->cmd_len - 1];
        tel->cmd[tel->cmd_len++] = byte;
        switch(prev_byte)
        {
            // Three-byte commands.
            case TELNET_WILL:
            case TELNET_WONT:
            case TELNET_DO:
            case TELNET_DONT:
                tel->cmd_ready = 1;
                break;
            // Sub-negotiation can span an arbitrary number of bytes.
            case TELNET_SB:
                break;
            default:
                // Something bad happened... start over.
                telnet_clear_cmd(tel);
                break;
        }
    } else if(tel->cmd_len < TELNET_MAX_COMMAND_SIZE) {
        tel->cmd[tel->cmd_len++] = byte;

        // Sub-negotiation is assumed, since that is the only command
        // that can be this long. Check if the most recent bytes are
        // IAC,SE. This ends sub-negotiation.
        if(tel->cmd[tel->cmd_len - 2] == TELNET_IAC &&
                tel->cmd[tel->cmd_len - 1] == TELNET_SE) {
            tel->cmd_ready = 1;
        }
    } else {
        // Boom. Ran out of space for the command :(
        telnet_clear_cmd(tel);
    }

    return 1;
}
