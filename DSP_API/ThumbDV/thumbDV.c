///*!   \file thumbdv.c
// *    \brief Functions required to communicate and decode packets from ThumbDV
// *
// *    \copyright  Copyright 2012-2014 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 26-MAY-2015
// *    \author     Ed Gonzalez
// *
// *
// */

/* *****************************************************************************
 *
 *  Copyright (C) 2014 FlexRadio Systems.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Contact Information:
 *  email: gpl<at>flexradiosystems.com
 *  Mail:  FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728
 *
 * ************************************************************************** */

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/prctl.h>

#include <netinet/in.h>

#include "common.h"
#include "datatypes.h"
#include "hal_buffer.h"

#include "vita_output.h"
#include "thumbDV.h"
#include "sched_waveform.h"
#include "ftd2xx.h"

#define AMBE3000_HEADER_LEN     4U
#define AMBE3000_START_BYTE     0x61U

#define AMBE3000_SPEECHD_HEADER_LEN 3U

#define AMBE3000_CTRL_PKT_TYPE      0x00
#define AMBE3000_SPEECH_PKT_TYPE    0x02
#define AMBE3000_CHAN_PKT_TYPE      0x01

#define BUFFER_LENGTH           400U
#define THUMBDV_MAX_PACKET_LEN  2048U

static pthread_t _read_thread;
BOOL _readThreadAbort = FALSE;

static uint32 _buffering_target = 1;

static pthread_rwlock_t _encoded_list_lock;
static BufferDescriptor _encoded_root;
static BOOL _encoded_buffering = TRUE;
static uint32 _encoded_count = 0;

static pthread_rwlock_t _decoded_list_lock;
static BufferDescriptor _decoded_root;
static BOOL _decoded_buffering = TRUE;
static uint32 _decoded_count = 0;

static void * _thumbDV_readThread( void * param );

static BufferDescriptor _thumbDVEncodedList_UnlinkHead( void ) {
    BufferDescriptor buf_desc = NULL;
    pthread_rwlock_wrlock( &_encoded_list_lock );


    if ( _encoded_root == NULL || _encoded_root->next == NULL ) {
        output( "Attempt to unlink from a NULL head" );
        pthread_rwlock_unlock( &_encoded_list_lock );
        return NULL;
    }

    if ( _encoded_buffering ) {
        pthread_rwlock_unlock( &_encoded_list_lock );
        return NULL;
    }

    if ( _encoded_root->next != _encoded_root )
        buf_desc = _encoded_root->next;

    if ( buf_desc != NULL ) {
        // make sure buffer exists and is actually linked
        if ( !buf_desc || !buf_desc->prev || !buf_desc->next ) {
            output( "Invalid buffer descriptor" );
            buf_desc = NULL;
        } else {
            buf_desc->next->prev = buf_desc->prev;
            buf_desc->prev->next = buf_desc->next;
            buf_desc->next = NULL;
            buf_desc->prev = NULL;

            if ( _encoded_count > 0 ) _encoded_count--;
        }
    } else {
        if ( !_encoded_buffering ) output( "Encoded list now buffering\n" );

        _encoded_buffering = TRUE;
    }

    pthread_rwlock_unlock( &_encoded_list_lock );
    return buf_desc;
}

static void _thumbDVEncodedList_LinkTail( BufferDescriptor buf_desc ) {
    pthread_rwlock_wrlock( &_encoded_list_lock );
    buf_desc->next = _encoded_root;
    buf_desc->prev = _encoded_root->prev;
    _encoded_root->prev->next = buf_desc;
    _encoded_root->prev = buf_desc;
    _encoded_count++;
    output("Encoded: %d \n",_encoded_count);

    if ( _encoded_count > _buffering_target ) {
        if ( _encoded_buffering ) output( "Encoded Buffering is now FALSE\n" );

        _encoded_buffering = FALSE;
    }

    pthread_rwlock_unlock( &_encoded_list_lock );
}

static BufferDescriptor _thumbDVDecodedList_UnlinkHead( void ) {
    BufferDescriptor buf_desc = NULL;
    pthread_rwlock_wrlock( &_decoded_list_lock );

    if ( _decoded_root == NULL || _decoded_root->next == NULL ) {
        output( "Attempt to unlink from a NULL head" );
        pthread_rwlock_unlock( &_decoded_list_lock );
        return NULL;
    }

    if ( _decoded_buffering ) {
        pthread_rwlock_unlock( &_decoded_list_lock );
        return NULL;
    }

    if ( _decoded_root->next != _decoded_root ) {
        buf_desc = _decoded_root->next;
    }

    if ( buf_desc != NULL ) {
        //output("0");
        // make sure buffer exists and is actually linked
        if ( !buf_desc || !buf_desc->prev || !buf_desc->next ) {
            output( "Invalid buffer descriptor" );
            buf_desc = NULL;
        } else {
            buf_desc->next->prev = buf_desc->prev;
            buf_desc->prev->next = buf_desc->next;
            buf_desc->next = NULL;
            buf_desc->prev = NULL;

            if ( _decoded_count > 0 ) {
            	_decoded_count--;
            	//output("Decoded Subtract : %d \n", _decoded_count);
            }
        }
    } else {
        if ( !_decoded_buffering )
            output( "DecodedList now Buffering \n" );

        _decoded_buffering = TRUE;
    }

    pthread_rwlock_unlock( &_decoded_list_lock );
    return buf_desc;
}

static void _thumbDVDecodedList_LinkTail( BufferDescriptor buf_desc ) {
    pthread_rwlock_wrlock( &_decoded_list_lock );
    buf_desc->next = _decoded_root;
    buf_desc->prev = _decoded_root->prev;
    _decoded_root->prev->next = buf_desc;
    _decoded_root->prev = buf_desc;

    _decoded_count++;
    if (_decoded_count > 10)
    {
    	//output("Decoded Count Added: %d \n", _decoded_count);
    }

    if ( _decoded_count > _buffering_target ) {
        if ( _decoded_buffering )
        {
        	output( "Decoded Buffering is now FALSE\n" );
        }

        _decoded_buffering = FALSE;
    }

    pthread_rwlock_unlock( &_decoded_list_lock );
}

static void delay( unsigned int delay ) {
    struct timespec tim, tim2;
    tim.tv_sec = 0;
    tim.tv_nsec = delay * 1000UL;
    nanosleep( &tim, &tim2 );
};

void thumbDV_flushLists(void)
{
    BufferDescriptor buf_desc = NULL;

    do
    {
        buf_desc = _thumbDVEncodedList_UnlinkHead();
        if ( buf_desc != NULL )
            hal_BufferRelease(&buf_desc);
    } while (buf_desc != NULL );


    do
    {
        buf_desc = _thumbDVDecodedList_UnlinkHead();
        if ( buf_desc != NULL )
            hal_BufferRelease(&buf_desc);
        	output("Delete Speech Data \n");
    } while (buf_desc != NULL );
}

void thumbDV_dump( char * text, unsigned char * data, unsigned int length ) {
    unsigned int offset = 0U;
    unsigned int i;

    output( "%s", text );
    output( "\n" );

    while ( length > 0U ) {
        unsigned int bytes = ( length > 16U ) ? 16U : length;

        output( "%04X:  ", offset );

        for ( i = 0U; i < bytes; i++ )
            output( "%02X ", data[offset + i] );

        for ( i = bytes; i < 16U; i++ )
            output( "   " );

        output( "   *" );

        for ( i = 0U; i < bytes; i++ ) {
            unsigned char c = data[offset + i];

            if ( isprint( c ) )
                output( "%c", c );
            else
                output( "." );
        }

        output( "*\n" );

        offset += 16U;

        if ( length >= 16U )
            length -= 16U;
        else
            length = 0U;
    }
}

static void thumbDV_writeSerial( FT_HANDLE handle , unsigned char * buffer, uint32 bytes )
{
    FT_STATUS status = FT_OK;
    DWORD written = 0;

    if ( handle != NULL )
    {
        status = FT_Write(handle, buffer, bytes, &written);

        if ( status != FT_OK || written != bytes ) {
            output( ANSI_RED "Could not write to serial port. status = %d\n", status );
            return;
        }
    }
    else
    {
        output( ANSI_RED "Could not write to serial port. Timeout\n" ANSI_WHITE );
    }

}

static int _check_serial( FT_HANDLE handle )
{

    unsigned char reset[5] = { 0x61, 0x00, 0x01, 0x00, 0x33 };
    thumbDV_writeSerial( handle, reset, 5 );

    int ret = thumbDV_processSerial(handle);

    if ( ret != 0 )
    {
        output( "Could not reset serial port FD = %d \n", handle );
        return -1;
    }

    unsigned char get_prodID[5] = {0x61, 0x00, 0x01, 0x00, 0x30 };
    thumbDV_writeSerial( handle, get_prodID, 5 );

    ret = thumbDV_processSerial(handle);

    if ( ret != 0 )
    {
        output( "Could not reset serial port FD = %d \n", handle );
        return -1;
    }


    return 0 ;
}

FT_HANDLE thumbDV_openSerial( FT_DEVICE_LIST_INFO_NODE device )
{
    //struct termios tty;
    FT_HANDLE handle = NULL;
    FT_STATUS status = FT_OK;
    UCHAR latency = 6;

    output("Trying to open serial port %s \n", device.SerialNumber);

    status = FT_OpenEx(device.SerialNumber, FT_OPEN_BY_SERIAL_NUMBER, &handle);

    if ( status != FT_OK || handle == NULL )
    {
        if ( device.SerialNumber )
            output("Error opening device %s - error 0x%X\n", device.SerialNumber, status);
        else
            output("Error opening device - error 0x%X\n", status);

        return NULL;
    }

    FT_SetBaudRate(handle, FT_BAUD_460800);

    // Set read and write timeout to 2seconds */
    FT_SetTimeouts(handle, 0, 0);


    FT_SetDataCharacteristics(handle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT_SetFlowControl(handle, FT_FLOW_NONE, 0, 0);

/*
    tty.c_cflag = ( tty.c_cflag & ~CSIZE ) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;

    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 5;

    tty.c_iflag &= ~( IXON | IXOFF | IXANY );

    tty.c_cflag |= ( CLOCAL | CREAD );

    tty.c_cflag &= ~( PARENB | PARODD );
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if ( tcsetattr( fd, TCSANOW, &tty ) != 0 ) {
        output( "ThumbDV: error %d from tcsetattr\n", errno );
        close( fd );
        return -1;
    }
*/
    if ( _check_serial(handle) != 0 ) {
        output( "Could not detect ThumbDV at 460800 Baud. Trying 230400\n" );
        FT_Close(handle);
        handle = (FT_HANDLE)NULL;
    } else {
        return handle;
    }

    status = FT_OpenEx(device.SerialNumber, FT_OPEN_BY_SERIAL_NUMBER, &handle);

    if ( status != FT_OK || handle == NULL )
    {
        if ( device.SerialNumber )
            output("Error opening device %s - error 0x%X\n", device.SerialNumber, status);
        else
            output("Error opening device - error 0x%X\n", status);

        return NULL;
    }

    FT_SetBaudRate(handle, FT_BAUD_230400 );

    FT_SetTimeouts(handle, 0, 0);


    FT_SetDataCharacteristics(handle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT_SetFlowControl(handle, FT_FLOW_NONE, 0, 0);
    FT_SetLatencyTimer(handle, latency);


    if ( _check_serial( handle ) != 0 ) {
        output( "Could not detect THumbDV at 230400 Baud\n" );
        FT_Close(handle);
        handle = NULL;
        return NULL;
    }

    return handle;
}

int thumbDV_processSerial( FT_HANDLE handle )
{
    unsigned char buffer[BUFFER_LENGTH];
    unsigned int respLen;
    //uint32 offset = 0;

    unsigned char packet_type;

    FT_STATUS status = FT_OK;
    DWORD rx_bytes = 0;
    DWORD tx_bytes = 0 ;
    DWORD event_word = 0;
    uint32 max_us_sleep = 100000; // 100 ms
    uint32 us_slept = 0;
    do
    {
        status = FT_GetStatus(handle, &rx_bytes, &tx_bytes, &event_word);

        if ( rx_bytes >= AMBE3000_HEADER_LEN )
            break;

        usleep(100);

        us_slept += 100;

        if ( us_slept > max_us_sleep )
        {
            output("TimeOut\n");
            return 1;
        }

    } while (rx_bytes < AMBE3000_HEADER_LEN && status == FT_OK );

    status = FT_Read(handle, buffer, AMBE3000_HEADER_LEN, &rx_bytes);

    if ( status != FT_OK || rx_bytes != AMBE3000_HEADER_LEN)
    {
        output( ANSI_RED "ThumbDV: Process serial. error when reading from the serial port, len = %d, status=%d\n" ANSI_WHITE, rx_bytes, status );
        return 1;
    }

    if ( buffer[0U] != AMBE3000_START_BYTE ) {
        output( ANSI_RED "ThumbDV: unknown byte from the DV3000, 0x%02X,  0x%02X,  0x%02X\n" ANSI_WHITE, buffer[0U], buffer[1U], buffer[2U] );
        return 1;
    }

    //offset = 0U;

    respLen = buffer[1U] * 256U + buffer[2U];
    //output("Length: %d \n", respLen);

//    us_slept = 0;
//    do
//    {
//        status = FT_GetStatus(handle, &rx_bytes, &tx_bytes, &event_word);
//
//        if ( rx_bytes >= respLen )
//            break;
//
//        usleep(1000);
//
//        us_slept += 1000 ;
//
//        if ( us_slept > max_us_sleep )
//        {
//            output("TimeOut\n");
//            return 1;
//        }
//
//    } while (rx_bytes < respLen && status == FT_OK);

    status = FT_Read(handle, buffer + AMBE3000_HEADER_LEN , respLen, &rx_bytes);

    if ( status != FT_OK || rx_bytes != respLen )
    {
        output( ANSI_RED "ThumbDV: Process serial. error when reading from the serial port, len = %d, status=%d\n" ANSI_WHITE, rx_bytes, status );
    }

    respLen += AMBE3000_HEADER_LEN;

    BufferDescriptor desc = NULL;
    packet_type = buffer[3U];

    //output("buffer [3] = %d \n", packet_type);

    //thumbDV_dump("Serial data", buffer, respLen);
    if ( packet_type == AMBE3000_CTRL_PKT_TYPE ) {
        thumbDV_dump( ANSI_YELLOW "Serial data" ANSI_WHITE, buffer, respLen );
    } else if ( packet_type == AMBE3000_CHAN_PKT_TYPE ) {
        desc = hal_BufferRequest( respLen, sizeof( unsigned char ) );
        memcpy( desc->buf_ptr, buffer, respLen );
        //thumbDV_dump(ANSI_BLUE "Coded Packet" ANSI_WHITE, buffer, respLen);
        /* Encoded data */
        _thumbDVEncodedList_LinkTail( desc );
    } else if ( packet_type == AMBE3000_SPEECH_PKT_TYPE ) {
    	//output("Response Length %d \n", respLen);
    	desc = hal_BufferRequest( respLen, sizeof( unsigned char ) );
    	memcpy( desc->buf_ptr, buffer, respLen );
    	//thumbDV_dump("SPEECH Packet", buffer, respLen);

   		/* Speech data */
   		_thumbDVDecodedList_LinkTail( desc );
    	}

        //memcpy(buffer,0,AMBE3000_HEADER_LEN);
    else {
        output( ANSI_RED "Unrecognized packet type 0x%02X ", packet_type );
        return 1;

    }


    return 0;
}



int thumbDV_decode( FT_HANDLE handle, unsigned char * packet_in, short * speech_out, uint8 bytes_in_packet) {

	//output("Decoded count @ decode: %d \n", _decoded_count);

    uint32 i = 0;
    struct timespec timer;
    int indicator;



    unsigned char full_packet[15] = {0};


    if ( packet_in != NULL && handle != NULL ) {
        full_packet[0] = 0x61;
        full_packet[1] = 0x00;
        full_packet[2] = 0x0B;
        full_packet[3] = 0x01;
        full_packet[4] = 0x01;
        full_packet[5] = 0x48;
        uint32 j = 0;

        //char p_in_size = sizeof(packet_in)/sizeof(packet_in[0]);

        //output("Packet in size: %d \n",p_in_size);

        for ( i = 0, j = 8  ; i < 9 ; i++ , j-- ) {
            full_packet[i + 6] = packet_in[i];
            //output("Packet in %d , %d \n",i,packet_in[i]);
        }

//        thumbDV_dump("Just AMBE", packet_in, 9);
//        thumbDV_dump("Encoded packet:", full_packet, 15);
        clock_gettime(CLOCK_MONOTONIC,&timer);
        thumbDV_writeSerial( handle, full_packet, 15 );
        thumbDV_processSerial(handle);
        indicator = msSince(timer);
        output("After Process time: %d \n", indicator);
        //output("Decoded count after channel write: %d \n", _decoded_count);
    }

    //delay(100);


    int32 samples_returned = 0;
    BufferDescriptor desc = _thumbDVDecodedList_UnlinkHead();
    uint32 samples_in_speech_packet = 0;
    uint32 length = 0;

    if ( desc != NULL) {

        length = ( ( ( unsigned char * )desc->buf_ptr )[1] << 8 ) + ( ( unsigned char * )desc->buf_ptr )[2];;

        if ( length != 0x142 ) {
            output( ANSI_YELLOW, "WARNING LENGHT DOESN'T Match %d " ANSI_WHITE, length );
            thumbDV_dump( "MISMATHCED", ( ( unsigned char * ) desc->buf_ptr ), desc->num_samples );
        }

        samples_in_speech_packet = ( ( unsigned char * )desc->buf_ptr )[5];

        unsigned char * idx = &( ( ( unsigned char * )desc->buf_ptr )[6] );
        uint32 i = 0;

        for ( i = 0; i < samples_in_speech_packet; i++, idx += 2 ) {
            speech_out[i] = ( idx[0] << 8 ) + idx[1];
        }

        samples_returned = samples_in_speech_packet;
        //output("Decoded count after unlink: %d \n", _decoded_count);

        if ( samples_returned != 160 ) output( "Rate Mismatch expected %d got %d\n", 160, samples_returned );

//        safe_free( desc );
        if ( desc != NULL ) {
        	//output("Deleting the decode list\n");
        	hal_BufferRelease(&desc);
        }
    } else {
        /* Do nothing for now */
    	//if ( samples_returned != 160 ) output( "Rate Mismatch expected %d got %d\n", 160, samples_returned );
    }
    return samples_returned;
}

int thumbDV_encode( FT_HANDLE handle, short * speech_in, unsigned char * packet_out, uint8 num_of_samples )
{
    unsigned char packet[THUMBDV_MAX_PACKET_LEN];
    uint16 speech_d_bytes = num_of_samples * sizeof( uint16 ); /* Should be 2 times the number of samples */

    /* Calculate length of packet NOT including the full header just the type field*/
    uint16 length = 0;
    /* Includes Channel Field and SpeechD Field Header */
    length += AMBE3000_SPEECHD_HEADER_LEN;
    length += speech_d_bytes;

    /* Will be used to write fields into packet */
    unsigned char * idx = &packet[0];

    *( idx++ ) = AMBE3000_START_BYTE;
    /* Length split into two bytes */
    *( idx++ ) = length >> 8;
    *( idx++ ) = length & 0xFF;
    /* SPEECHD Type */
    *( idx++ ) = AMBE3000_SPEECH_PKT_TYPE;
    /* Channel0 Identifier */
    *( idx++ ) = 0x40;
    /* SPEEECHD Identifier */
    *( idx++ ) = 0x00;
    /* SPEECHD No of Samples */
    *( idx++ ) = num_of_samples;
    uint32 i = 0;
//    output("Num of Samples = 0x%X\n", num_of_samples);

#ifdef WOOT
    output( "Encode Packet Header = " );
    unsigned char * p = &packet[0];
    i = 0;

    for ( i = 0 ; i < 7 ; i++ ) {

        output( "%02X ", *p );
        p++;
    }

    output( "\n" );

#endif
    //memcpy(idx, speech_in, speech_d_bytes);
    i = 0;

    for ( i = 0 ; i < num_of_samples ; i++, idx += 2 ) {
        idx[0] = speech_in[i] >> 8;
        idx[1] = ( speech_in[i] & 0x00FF ) ;
    }

    if ( handle != NULL )
        thumbDV_writeSerial( handle, packet, length + AMBE3000_HEADER_LEN );

    thumbDV_processSerial(handle);

    int32 samples_returned = 0;
    BufferDescriptor desc = _thumbDVEncodedList_UnlinkHead();

    if ( desc != NULL ) {
        memcpy( packet_out, desc->buf_ptr + 6, desc->sample_size * ( desc->num_samples - 6 ) );
        samples_returned = desc->num_samples - 6;
        //safe_free( desc );
        hal_BufferRelease(&desc);
        //thumbDV_dump(ANSI_BLUE "Coded Packet" ANSI_WHITE, packet_out, desc->num_samples - 6);

    } else {
        /* Do nothing for now */
    }

    return samples_returned;

}

static void _connectSerial( FT_HANDLE * ftHandle )
{
    int i = 0 ;
    struct timespec timer;
    int indicator;

    output("ConnectSerial\n");

    DWORD numDevs = 0;
    FT_DEVICE_LIST_INFO_NODE *devInfo = NULL;
    FT_STATUS status = FT_OK;

    do {


        status = FT_CreateDeviceInfoList(&numDevs);

        devInfo = (FT_DEVICE_LIST_INFO_NODE *) safe_malloc(sizeof(FT_DEVICE_LIST_INFO_NODE) * numDevs);

        status = FT_GetDeviceInfoList(devInfo, &numDevs);

        for ( i = 0 ; i < numDevs ; i++ )
        {

            *ftHandle = thumbDV_openSerial(devInfo[i]);

            if ( *ftHandle != NULL )
            {
                /* We opened a valid port and detected the ThumbDV */
                break;
            }
        }
        safe_free(devInfo);

        if ( *ftHandle == NULL ) {
            output( "Could not open serial. Waiting 1 second before trying again.\n" );
            usleep( 1000 * 1000 );
        }
    } while ( *ftHandle == NULL ) ;

    //sched_waveform_setHandle(&ftHandle);

//    unsigned char reset[5] = { 0x61, 0x00, 0x01, 0x00, 0x33 };
//    thumbDV_writeSerial( *ftHandle, reset, 5 );
//    /* Block until we get data from serial port after reset */
//    thumbDV_processSerial( *ftHandle );
//
//    unsigned char reset_softcfg[11] = {0x61, 0x00, 0x07, 0x00, 0x34, 0x05, 0x03, 0xEB, 0xFF, 0xFF, 0xFF};
//    thumbDV_writeSerial(*ftHandle, reset_softcfg, 11);
//    thumbDV_processSerial(*ftHandle);

    unsigned char disable_parity[6] = {0x61, 0x00, 0x02, 0x00, 0x3F, 0x00};
    clock_gettime(CLOCK_MONOTONIC, &timer);
    thumbDV_writeSerial( *ftHandle, disable_parity, 6 );
    thumbDV_processSerial( *ftHandle );
    indicator = msSince(timer);
    output("Time for serial: %d \n", indicator);

    //unsigned char get_prodID[5] = {0x61, 0x00, 0x01, 0x00, 0x30 };
    unsigned char get_version[5] = {0x61, 0x00, 0x01, 0x00, 0x31};
    unsigned char read_cfg[5] = {0x61, 0x00, 0x01, 0x00, 0x37};
    unsigned char dstar_mode[17] = {0x61, 0x00, 0x0D, 0x00, 0x0A, 0x01, 0x30, 0x07, 0x63, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48};

//    thumbDV_writeSerial( *ftHandle, get_prodID, 5 );
//    thumbDV_processSerial(*ftHandle);


    clock_gettime(CLOCK_MONOTONIC, &timer);
    thumbDV_writeSerial( *ftHandle, get_version, 5 );
    thumbDV_processSerial(*ftHandle);
    indicator = msSince(timer);
    output("Time for serial: %d \n", indicator);

    thumbDV_writeSerial( *ftHandle, read_cfg, 5 );
    thumbDV_processSerial(*ftHandle);

    thumbDV_writeSerial( *ftHandle, dstar_mode, 17 );
    thumbDV_processSerial(*ftHandle);
    //thumbDV_processSerial(*ftHandle);

    //delay(100);

////    /* Init */
    unsigned char pkt_init[6] = { 0x61, 0x00, 0x02, 0x00, 0x0B, 0x07 };
    thumbDV_writeSerial( *ftHandle, pkt_init, 6 );
    thumbDV_processSerial(*ftHandle);

    /* PKT GAIN - set to 0dB */
    unsigned char pkt_gain[7] = { 0x61, 0x00, 0x03, 0x00, 0x4B, 0x00, 0x00 };
    thumbDV_writeSerial( *ftHandle, pkt_gain, 7 );
    thumbDV_processSerial(*ftHandle);

    /* Companding off so it uses 16bit linear */
    unsigned char pkt_compand[6] = { 0x61, 0x00, 0x02, 0x00, 0x32, 0x00 };
    thumbDV_writeSerial( *ftHandle, pkt_compand, 6 );
    thumbDV_processSerial(*ftHandle);

    unsigned char test_coded[15] =  {0x61, 0x00 , 0x0B , 0x01 , 0x01 , 0x48 , 0x5E , 0x83 , 0x12 , 0x3B , 0x98 , 0x79 , 0xDE , 0x13 , 0x90};
    thumbDV_writeSerial( *ftHandle, test_coded, 15 );
    thumbDV_processSerial(*ftHandle);


    unsigned char pkt_fmt[7] = {0x61, 0x00, 0x3, 0x00, 0x15, 0x00, 0x00};
    thumbDV_writeSerial( *ftHandle, pkt_fmt, 7 );
    thumbDV_processSerial(*ftHandle);

}


//static void * _thumbDV_readThread( void * param )
//{
//    int ret;
//    DWORD rx_bytes;
//    DWORD tx_bytes;
//    DWORD event_dword;
//
//    FT_STATUS status = FT_OK;
//    FT_HANDLE handle = *( FT_HANDLE * )param;
//    EVENT_HANDLE event_handle;
//
//    prctl(PR_SET_NAME, "DV-Read");
//
//    pthread_mutex_init(&event_handle.eMutex, NULL);
//    pthread_cond_init(&event_handle.eCondVar, NULL);
//
//    while ( !_readThreadAbort )
//    {
//    	//output("Reading... \n");
//        // Setup RX or Status change event notification
//        status = FT_SetEventNotification(handle, FT_EVENT_RXCHAR , (PVOID)&event_handle);
//        //output("FT Status 1 %d \n",status);
//
//        struct timespec timeout;
//        clock_gettime(CLOCK_REALTIME, &timeout);
//
//        timeout.tv_sec += 2; // 2 second timeout
//
//        // Will block until
//        pthread_mutex_lock(&event_handle.eMutex);
//        pthread_cond_timedwait(&event_handle.eCondVar, &event_handle.eMutex, &timeout);
//        pthread_mutex_unlock(&event_handle.eMutex);
//
//        rx_bytes = 0;
//        status = FT_GetStatus(handle, &rx_bytes, &tx_bytes, &event_dword);
//        //output("FT Status 2 %d\n",status);
//
//        if ( status != FT_OK )
//        {
//            fprintf( stderr, "ThumbDV: error from status, status=%d\n", status );
//
//            /* Set invalid FD in sched_waveform so we don't call write functions */
//            handle = NULL;
//            sched_waveform_setHandle(&handle);
//            /* This function hangs until a new connection is made */
//            _connectSerial( &handle );
//            /* Update the sched_waveform to new valid serial */
//            sched_waveform_setHandle( &handle );
//        }
//        else if ( rx_bytes >= AMBE3000_HEADER_LEN )
//        {
//        	//output("RX Bytes: %d \n",rx_bytes);
//            //ret = thumbDV_processSerial( handle );
////            output("Decoded count in init: %d \n",_decoded_count);
//
//        }
//    }
//
//    output( ANSI_YELLOW "thumbDV_readThread has exited\n" ANSI_WHITE );
//    return 0;
//}

void thumbDV_init( FT_HANDLE * handle ) {
    pthread_rwlock_init( &_encoded_list_lock, NULL );
    pthread_rwlock_init( &_decoded_list_lock, NULL );

    pthread_rwlock_wrlock( &_encoded_list_lock );
    _encoded_root = ( BufferDescriptor )safe_malloc( sizeof( buffer_descriptor ) );
    memset( _encoded_root, 0, sizeof( buffer_descriptor ) );
    _encoded_root->next = _encoded_root;
    _encoded_root->prev = _encoded_root;
    pthread_rwlock_unlock( &_encoded_list_lock );

    pthread_rwlock_wrlock( &_decoded_list_lock );
    _decoded_root = ( BufferDescriptor )safe_malloc( sizeof( buffer_descriptor ) );
    memset( _decoded_root, 0, sizeof( buffer_descriptor ) );
    _decoded_root->next = _decoded_root;
    _decoded_root->prev = _decoded_root;
    pthread_rwlock_unlock( &_decoded_list_lock );

    _connectSerial( handle );

    //pthread_create( &_read_thread, NULL, &_thumbDV_readThread, handle );

//    struct sched_param fifo_param;
//    fifo_param.sched_priority = 30;
//    pthread_setschedparam( _read_thread, SCHED_FIFO, &fifo_param );

}
