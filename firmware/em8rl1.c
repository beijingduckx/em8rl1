//
// firmware for Cypress EZ-USB FX2LP
//  CZ-8RL1 emulator
//
#include "Fx2.h"
#include "fx2regs.h"
#include "syncdly.h"
#include <stdint.h>

typedef enum
{
    TAPE_MODE_PLAY,
    TAPE_MODE_REC,
    TAPE_MODE_STOP,
} tape_mode_t;

typedef enum
{
    PC_SENSOR_CHANGE,
    PC_STATUS_CHANGE,
    PC_REQUEST,
    PC_USB_RATE_CHANGE,
} pc_response_t;

typedef enum
{
    COM_STATE_WAIT_LEADER,
    COM_STATE_IN_LEADER,
    COM_STATE_IN_BITS,
} command_state_t;

typedef enum
{
    COM_EJECT = 0x00,
    COM_STOP = 0x01,
    COM_PLAY = 0x02,
    COM_FF = 0x03,
    COM_REW = 0x04,
    COM_AFF = 0x05,
    COM_AREW = 0x06,
    COM_REC = 0x0a,
    COM_STATUS = 0x80,
    COM_SENSOR = 0x81,
} cas_command_t;

typedef enum
{
    TAPE_SAMPLE_48K = 0,
    TAPE_SAMPLE_44K = 1,
    TAPE_SAMPLE_44K_ALT1 = 2,
    TAPE_SAMPLE_32K = 3,
} tape_sample_rate_t;

#define SENSOR_TAPE_RUN 0x1
#define SENSOR_TAPE_SET 0x2
#define SENSOR_WRITE_PROT 0x4
#define STATUS_BREAK 0xff

#define IO_STROBE 0x4
#define IO_STATUS 0x10
#define IO_BUSY 0x20

volatile uint8_t tape_mode = TAPE_MODE_STOP;
volatile uint8_t bit_index = 0;
volatile uint8_t tape_value = 0;
volatile uint8_t tape_counter;
volatile uint8_t is_tape_sample_timer_enabled = 0;
volatile command_state_t command_state;

uint8_t command_byte;
uint8_t bit_count;
uint8_t command_task;

uint8_t cached_status;
uint8_t cached_sensor;
uint8_t usb_sample_rate;

#define US_TO_50US_COUNT(us) ((us) / 50)

volatile uint8_t S0us_count;

void GpifInit(void);

void initialize()
{
    // ----------------------------------------------------------------------
    // CPU Clock
    // ----------------------------------------------------------------------
    // bit7:6 -
    // bit5   1=PortC RD#/WR# Strobe enable
    // bit4:3 00=12MHz, 01=24MHz, 10=48MHz, 11=reserved
    // bit2   1=CLKOUT inverted
    // bit1   1=CLKOUT enable
    // bit0   1=reset
    CPUCS = 0x10; // 0b0001_0000; 48MHz
    SYNCDELAY;
    CKCON = 0x00;
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // Interface Config
    // ----------------------------------------------------------------------
    // bit7   1=Internal clock, 0=External
    // bit6   1=48MHz, 0=30MHz
    // bit5   1=IFCLK out enable
    // bit4   1=IFCLK inverted
    // bit3   1=Async, 0=Sync
    // bit2   1=GPIF GSTATE out enable
    // bit1:0 00=Ports, 01=Reserved, 10=GPIF, 11=Slave FIFO
    // IFCONFIG = 0x00; // 0b0000_0000; Ports
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // Chip Revision Control
    // ----------------------------------------------------------------------
    REVCTL = 0x03; // Recommended setting.
    SYNCDELAY;

    GpifInit();
    // ----------------------------------------------------------------------
    // EP Config
    // ----------------------------------------------------------------------
    // bit7   1=Valid
    // bit6   1=IN, 0=OUT
    // bit5:4 00=Invalid, 01=Isochronous, 10=Bulk(default), 11=Interrupt
    // bit3   1=1024bytes buffer(EP2,6 only), 0=512bytes
    // bit2   -
    // bit1:0 00=Quad, 01=Invalid, 10=Double, 11=Triple
    EP1OUTCFG = 0xa0; // 0b1010_0000  // Valid, Bulk-OUT
    SYNCDELAY;
    EP1INCFG = 0xa0; // Valid, Bulk-IN
    SYNCDELAY;
    EP2CFG &= 0x7f; // disable
    SYNCDELAY;
    EP4CFG &= 0xa2; // Bulk-OUT, 512 byte, Double buffer
    SYNCDELAY;
    EP6CFG = 0xe2; // 0b1110_0010; Bulk-IN, 512bytes Double buffer
    SYNCDELAY;
    EP8CFG &= 0x7f; // Bluk-OUT, 512bytes, Double buffer
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // Autopointer
    // ----------------------------------------------------------------------
    AUTOPTRSETUP = 0x7; // Enable both auto-pointer
    SYNCDELAY;
    // ----------------------------------------------------------------------
    // Start EP1
    // ----------------------------------------------------------------------
    EP1OUTBC = 0x1; // Any value enables EP1 transfer
    SYNCDELAY;
    // ----------------------------------------------------------------------
    // Start EP4
    // ----------------------------------------------------------------------
    EP4BCL = 0x1; // Any value enables EP4 transfer
    SYNCDELAY;

    // ----------------------------------------------------------------------
    // IO Port
    // ----------------------------------------------------------------------
    OEA = 0xF0;
    IOA = 0x00;
    PORTACFG = 0x02; // PA.1 = INT1

    OED = 0x03;
    IOD = 0x00;

    TCON = 0x05;

    // ----------------------------------------------------------------------
    // GPIF
    // ----------------------------------------------------------------------
    EP2FIFOCFG = 0x04;
    SYNCDELAY;
    EP4FIFOCFG = 0x04;
    SYNCDELAY;
    EP6FIFOCFG = 0x04;
    SYNCDELAY;
    EP8FIFOCFG = 0x04;
    SYNCDELAY;

    EIE = 0x04;    // Enable INT4
    GPIFIE = 0x02; // GPIFWF
    SYNCDELAY;
    INTSETUP = 0x02; // INT4 source is GPIF

    // ----------------------------------------------------------------------
    // Timer
    // ----------------------------------------------------------------------
    TMOD = 0x22;  // GATE1=0, CT1 =0, T1: Mode2,  GATE0=0 , C/T0=0 (CLKOUT Source), Mode2: 8bit with autoload
    CKCON = 0x08; //  TM1: CLKOUT/12(4MHz)  TM0: CLKOUT/4 (12MHz)
}

void start_tape_sample_timer(void)
{
    if (is_tape_sample_timer_enabled == 0)
    {
        is_tape_sample_timer_enabled = 1;
        tape_counter = 0;
        bit_index = 0;

        if (tape_mode == TAPE_MODE_REC)
        {
            AUTOPTRH2 = MSB(&EP6FIFOBUF);
            AUTOPTRL2 = LSB(&EP6FIFOBUF);
        }
        else if (tape_mode == TAPE_MODE_PLAY)
        {
            AUTOPTRH2 = MSB(&EP4FIFOBUF);
            AUTOPTRL2 = LSB(&EP4FIFOBUF);
        }

        if (usb_sample_rate == TAPE_SAMPLE_48K)
        {
            CKCON = 0x08;                     //  TM1: CLKOUT/12(4MHz)  TM0: CLKOUT/4 (12MHz)
            TL0 = (unsigned char)(256 - 250); // 12M / 250 = 48kHz
            TH0 = (unsigned char)(256 - 250);
        }
        else if (usb_sample_rate == TAPE_SAMPLE_44K)
        {
            CKCON = 0x00;                    //  TM1: CLKOUT/12(4MHz)  TM0: CLKOUT/12 (4MHz)
            TL0 = (unsigned char)(256 - 90); // 4M / 90 = 44.444kHz
            TH0 = (unsigned char)(256 - 90);
        }
        else if(usb_sample_rate == TAPE_SAMPLE_44K_ALT1)
        {
            // another 44kHz
            CKCON = 0x00;                    //  TM1: CLKOUT/12(4MHz)  TM0: CLKOUT/12 (4MHz)
            TL0 = (unsigned char)(256 - 91); // 4M / 91 = 43.956kHz
            TH0 = (unsigned char)(256 - 91);
        }
        else if(usb_sample_rate == TAPE_SAMPLE_32K)
        {
            CKCON = 0x00;                    //  TM1: CLKOUT/12(4MHz)  TM0: CLKOUT/12 (4MHz)
            TL0 = (unsigned char)(256 - 125); // 4M / 125 = 32kHz
            TH0 = (unsigned char)(256 - 125);
        }
        TCON |= 0x10; // TR0=1 : start Timer0
    }
}

void stop_tape_sample_timer(void)
{
    TCON &= ~(0x10); // TR0=0 : stop Timer0
    is_tape_sample_timer_enabled = 0;
}

void tape_sample_timer_overflow_int(void) __interrupt(1)
{
    if (tape_mode == TAPE_MODE_REC)
    {
        tape_value <<= 1;
        tape_value |= IOA & 0x01;
        bit_index++;
        if (bit_index == 8)
        {
            if (!(EP2468STAT & bmEP6FULL))
            {
                //*current_tape_out = value;
                EXTAUTODAT2 = tape_value;
                // current_tape_out++;
                tape_counter++;
                if (tape_counter >= 128)
                {
                    EP6BCH = 0;
                    SYNCDELAY;
                    SYNCDELAY;
                    EP6BCL = tape_counter;
                    SYNCDELAY;
                    // current_tape_out = EP6FIFOBUF;
                    AUTOPTRH2 = MSB(&EP6FIFOBUF);
                    AUTOPTRL2 = LSB(&EP6FIFOBUF);
                    tape_counter = 0;
                }
            }
            bit_index = 0;
            tape_value = 0;
        }
    }
    else if (tape_mode == TAPE_MODE_PLAY)
    { // LOAD
        if (!(EP2468STAT & bmEP4EMPTY))
        {
            if (bit_index == 0)
            {
                tape_value = EXTAUTODAT2;
                tape_counter++;
                if (tape_counter == EP4BCL)
                {
                    AUTOPTRH2 = MSB(&EP4FIFOBUF);
                    AUTOPTRL2 = LSB(&EP4FIFOBUF);
                    EP4BCL = 0; // arm EP4
                    tape_counter = 0;
                }
            }
            IOD = (((tape_value)&0x80) >> 7);
            tape_value <<= 1;
            bit_index++;
            if (bit_index == 8)
            {
                bit_index = 0;
            }
        }
    }
    else
    {
        if (!(EP2468STAT & bmEP4EMPTY))
        {
            // just discard OUT packets from PC
            EP4BCL = 0x00; // clear state
            SYNCDELAY;
        }
    }
}

void S0us_timer_overflow_int(void) __interrupt(3)
{
    S0us_count++;
}

inline void start_50us_timer(void)
{
    S0us_count = 0;
    TL1 = (unsigned char)(256 - 200); //  200count / 4MHz = 50usec
    TH1 = (unsigned char)(256 - 200);
    TCON |= 0x40; // TR1=1 : start Timer1
}

inline void stop_50us_timer(void)
{
    TCON &= ~(0x40); // TR1=0 : stop Timer0
}

void send_response_bit(uint8_t bit)
{
    uint8_t wait_time;

    IOA |= IO_STATUS;
    if (bit)
    {
        wait_time = US_TO_50US_COUNT(750);
    }
    else
    {
        wait_time = US_TO_50US_COUNT(250);
    }
    start_50us_timer();
    while (S0us_count < wait_time)
        ;
    IOA &= ~(IO_STATUS);
    stop_50us_timer();
    start_50us_timer();
    while (S0us_count < US_TO_50US_COUNT(250))
        ;
    stop_50us_timer();
}

inline int wait_strobe(void)
{
    uint16_t index;

    // wait for 1sec
    for (index = 0; index < 1000; index++)
    {
        start_50us_timer();
        while (S0us_count < US_TO_50US_COUNT(1000))
        {
            if (IOA & IO_STROBE)
            {
                stop_50us_timer();
                return 0;
            }
        }
        stop_50us_timer();
    }

    return -1;
}

void send_response(uint8_t response)
{
    uint8_t index;

    IOA |= IO_BUSY;

    if (wait_strobe() < 0)
    {
        IOA &= ~(IO_BUSY);
        return;
    }
    start_50us_timer();
    while (S0us_count < US_TO_50US_COUNT(1000))
        ; // wait 1msec
    stop_50us_timer();
    IOA &= ~(IO_BUSY);
    start_50us_timer();
    while (S0us_count < US_TO_50US_COUNT(400))
        ; // wait 400usec
    stop_50us_timer();

    // send leader part
    IOA &= ~(IO_STATUS);
    start_50us_timer();
    while (S0us_count < US_TO_50US_COUNT(1000))
        ; //  wait 1msec
    stop_50us_timer();

    // send status byte
    for (index = 0; index < 8; index++)
    {
        send_response_bit(response & 0x80);
        response <<= 1;
    }
    IOA |= IO_STATUS;
}

inline int wait_EP1_ready(void)
{
    uint16_t index;

    // wait for 1sec
    for (index = 0; index < 1000; index++)
    {
        start_50us_timer();
        while (S0us_count < US_TO_50US_COUNT(1000))
        {
            if (!(EP1INCS & bmEPBUSY))
            {
                stop_50us_timer();
                return 0;
            }
        }
        stop_50us_timer();
    }
    return -1;
}

inline void send_pc_command(uint8_t command)
{
    wait_EP1_ready();
    *EP1INBUF = command;
    EP1INBC = 1; // Start IN transfer
}

void command_received(uint8_t command)
{
    uint8_t response = command;

    switch (command)
    {
    case COM_EJECT:
    case COM_STOP:
    case COM_PLAY:
    case COM_FF:
    case COM_REW:
    case COM_AFF:
    case COM_AREW:
    case COM_REC:
        send_pc_command(command);
        // cached_status = command;
        break;

    case COM_STATUS:
        response = cached_status;
        send_response(response);
        break;

    case COM_SENSOR:
        response = cached_sensor;
        send_response(response);
        break;
    }

    if (command == COM_PLAY)
    {
        tape_mode = TAPE_MODE_PLAY;
        start_tape_sample_timer();
    }
    else if (command == COM_REC)
    {
        tape_mode = TAPE_MODE_REC;
        start_tape_sample_timer();
    }
    else if (command != COM_STATUS && command != COM_SENSOR)
    {
        if (tape_mode == TAPE_MODE_REC)
        {
            uint8_t index;
            // 10msec extra wait for making blank
            for (index = 0; index < 10; index++)
            {
                start_50us_timer();
                while (S0us_count < US_TO_50US_COUNT(1000))
                    ; //  wait 1msec
                stop_50us_timer();
            }
            stop_tape_sample_timer();
            if (bit_index != 0)
            {
                EXTAUTODAT2 = tape_value;
                tape_counter++;
            }
            if (tape_counter != 0)
            {
                EP6BCH = 0;
                SYNCDELAY;
                EP6BCL = tape_counter;
                SYNCDELAY;
            }
        }
        else
        {
            //
        }
        tape_mode = TAPE_MODE_STOP;
    }

    return;
}

void init_command_task(void)
{
    command_task = 0;
    command_byte = 0;
    bit_count = 0;
    command_state = COM_STATE_WAIT_LEADER;

    cached_sensor = 0x80;
}

inline void feed_command_bit(uint8_t bit_value)
{
    command_byte <<= 1;
    command_byte |= bit_value;

    bit_count++;
    if (bit_count == 8)
    {
        command_state = COM_STATE_WAIT_LEADER;
        command_task = 1;
    }
}

void command_low_int(void) __interrupt(2)
{
    if (command_state == COM_STATE_WAIT_LEADER)
    {
        command_state = COM_STATE_IN_LEADER;
        bit_count = 0;
    }
    else
    {
        // Stop timer2 & check timer value
        stop_50us_timer();
        if(S0us_count > US_TO_50US_COUNT(1000))
        {
            init_command_task();   // Timeout
        }
        else if (S0us_count > US_TO_50US_COUNT(300))
        {
            feed_command_bit(1);
        }
        else
        {
            feed_command_bit(0);
        }
        command_state - COM_STATE_IN_BITS;
    }
}

void command_high_int(void) __interrupt(10)
{
    if (command_state == COM_STATE_WAIT_LEADER)
    {
        return;
    }
    start_50us_timer();

    EXIF = 0x0;
    // GPIFIRQ = 0x02;
    INT4CLR = 0x01;
}

void process_usb_command(void)
{
    uint8_t *src = EP1OUTBUF;
    uint8_t len = EP1OUTBC;

    switch (*(src++))
    {
    case PC_SENSOR_CHANGE:
        cached_sensor = *src;
        break;

    case PC_STATUS_CHANGE:
        cached_status = *src;
        break;

    case PC_REQUEST:
        send_response(*src);
        break;

    case PC_USB_RATE_CHANGE:
        stop_tape_sample_timer();
        usb_sample_rate = *src;
        break;
    }
    EP1OUTBC = 0x01;
    SYNCDELAY;
}

void main()
{
    initialize();
    init_command_task();
    cached_sensor = 0x80;
    cached_status = 0x80;
    usb_sample_rate = TAPE_SAMPLE_48K;

    IOA |= IO_STATUS; // BUSY=L, STATUS=H
    IE = 0x8F;        // Enable global interrupt, Enable Timer1, Timer0, INT0, INT1

    // start GPIF (COMMAND line polling)
    XGPIFSGLDATLX = 0x01;

    for (;;)
    {
        if (command_task)
        {
            command_task = 0;
            command_received(command_byte);
        }
        if (!(EP01STAT & 2))
        {
            process_usb_command();
        }
    }
}
