#include <osapi.h>

#include "gpio_util.h"
#include "ring_buffer.h"
#include "pins.h"

#define DEBUG_IGNORE_ACKNOWLEDGE_BIT true

enum state {
    IDLE,
    START,
    STOP,
    SEND_ADDRESS,
    SEND_DATA,
    RECEIVE_DATA,
    SEND_READ_WRITE_BIT,
    WAIT_FOR_ACKNOWLEDGE,
    SEND_ACKNOWLEDGE,
    SEND_NO_ACKNOWLEDGE
};
enum state i2c_master_state = IDLE; //what the master is currently doing
static enum state next_state = IDLE; //what the master is doing next (only used after some states)

ring_buffer_t i2c_master_receive_buffer = {.start=0, .end=0};
static uint8 next_byte_to_send = 0;

ring_buffer_t i2c_master_send_buffer = {.start=0, .end=0};
static int receive_counter = 0; //counts how many bytes need to be received
static uint8 current_receiving_byte = 0;

static int address = 0; //address of the slave with which the master is communicating with

static int bit_counter = 0;

static bool wait_one_tick = false; //set to true when the master has to wait for one tick, so that the slave can react

// The timer cycles through 4 steps. Step 0 and 2 change the clock signal.
// Step 1 and 3 are in the middle of the edges of the clock signal. Here the data pin is manipulated.
static int timer_cycle = 0;

static int i2c_master_wait_counter = 0;

// #define I2C_MASTER_DEBUG

void i2c_master_timer() {
    if (i2c_master_wait_counter > 0) {
        i2c_master_wait_counter--;
        return;
    }
    if (timer_cycle == 0 && i2c_master_state != IDLE) {
        pin_i2c_write(PIN_I2C_SCL, 0);
    } else if (timer_cycle == 1) { //set SDA to send data
        switch (i2c_master_state) {
            case IDLE:
                // checks if send buffer is not empty
                if (i2c_master_send_buffer.end != i2c_master_send_buffer.start || receive_counter > 0) {
                    i2c_master_state = START;
                }
                break;
            case SEND_ADDRESS:
                // writes current bit of address at each clock cycle
                pin_i2c_write(PIN_I2C_SDA, (address & (1 << (6 - bit_counter))) > 0);
                bit_counter++;
                if (bit_counter == 7) {
                    i2c_master_state = SEND_READ_WRITE_BIT;
                    bit_counter = 0;
                }
                break;
            case SEND_READ_WRITE_BIT:
                if (i2c_master_send_buffer.end != i2c_master_send_buffer.start) {
                    pin_i2c_write(PIN_I2C_SDA, 0); //send write bit
                    i2c_master_state = WAIT_FOR_ACKNOWLEDGE;
                    next_state = SEND_DATA;
                    wait_one_tick = true;
                } else {
                    pin_i2c_write(PIN_I2C_SDA, 1); //send read bit
                    i2c_master_state = WAIT_FOR_ACKNOWLEDGE;
                    next_state = RECEIVE_DATA;
                    wait_one_tick = true;
                }
                break;
            case WAIT_FOR_ACKNOWLEDGE:
                // preperation for acknowledge from slave
                pin_i2c_write(PIN_I2C_SDA, 1);
                break;
            case SEND_DATA:
                if (bit_counter == 0) {
                    next_byte_to_send = ring_buffer_read_one_byte(&i2c_master_send_buffer);
#ifdef I2C_MASTER_DEBUG
                    os_printf_plus("i2c_master sending byte: %c  %d\n", next_byte_to_send, next_byte_to_send);
#endif
                }
                pin_i2c_write(PIN_I2C_SDA, (next_byte_to_send & (1 << (7 - bit_counter))) > 0);
                bit_counter++;
                if (bit_counter == 8) {
                    bit_counter = 0;
                    i2c_master_state = WAIT_FOR_ACKNOWLEDGE;
                    wait_one_tick = true;
                    if (i2c_master_send_buffer.end != i2c_master_send_buffer.start) {
                        next_state = SEND_DATA;
                    } else {
                        next_state = STOP;
                    }
                }
                break;
            case RECEIVE_DATA:
                if (bit_counter == 0) {
                    pin_i2c_write(PIN_I2C_SDA, 1);
                    current_receiving_byte = 0;
                }
                break;
            case SEND_ACKNOWLEDGE:
#ifdef I2C_MASTER_DEBUG
                os_printf_plus("i2c_master sending ACK\n");
#endif
                pin_i2c_write(PIN_I2C_SDA, 0);
                i2c_master_state = next_state;
                wait_one_tick = true;
                break;
            case SEND_NO_ACKNOWLEDGE:
#ifdef I2C_MASTER_DEBUG
                os_printf_plus("i2c_master sending NACK\n");
#endif
                pin_i2c_write(PIN_I2C_SDA, 1);
                i2c_master_state = next_state;
                wait_one_tick = true;
                break;
            case STOP:
                pin_i2c_write(PIN_I2C_SDA, 0);
                i2c_master_wait_counter = 5;
                break;
            default:
                break;
        }
    } else if (timer_cycle == 2 && i2c_master_state != IDLE) {
        pin_i2c_write(PIN_I2C_SCL, 1);
    } else if (timer_cycle == 3) { //set SDA to create start or stop condition or read from SDA
        if (wait_one_tick) {
            wait_one_tick = false;
        } else {
            switch (i2c_master_state) {
                case START:
                    if (i2c_master_send_buffer.end != i2c_master_send_buffer.start || receive_counter > 0) {
                        pin_i2c_write(PIN_I2C_SDA, 0); //create start condition
                        i2c_master_state = SEND_ADDRESS;
                        i2c_master_wait_counter = 5;
                    }
                    break;
                case WAIT_FOR_ACKNOWLEDGE: {
                    int acknowledge_bit = pin_i2c_read(PIN_I2C_SDA);
                    if (acknowledge_bit == 0 || DEBUG_IGNORE_ACKNOWLEDGE_BIT) {
#ifdef I2C_MASTER_DEBUG
                        os_printf_plus("i2c_master received ACK\n");
#endif
                        i2c_master_state = next_state;
                    } else {
#ifdef I2C_MASTER_DEBUG
                        os_printf_plus("i2c_master received NACK\n");
#endif
                        i2c_master_state = STOP; //abort transmission
                    }
                }
                    break;
                case RECEIVE_DATA: {
                    int received_bit = pin_i2c_read(PIN_I2C_SDA);
                    current_receiving_byte = current_receiving_byte | (received_bit << (7 - bit_counter));
                    // os_printf_plus("i2c_master received bit %d: %d\n", bit_counter, received_bit);
                    bit_counter++;
                    if (bit_counter == 8) {
#ifdef I2C_MASTER_DEBUG
                        os_printf_plus("i2c_master received byte: %d  %c\n", current_receiving_byte,
                                       current_receiving_byte);
#endif
                        bit_counter = 0;
                        ring_buffer_write_one_byte(&i2c_master_receive_buffer, current_receiving_byte);
                        receive_counter--;
                        if (receive_counter > 0) {
                            i2c_master_state = SEND_ACKNOWLEDGE;
                            next_state = RECEIVE_DATA;
                        } else {
                            i2c_master_state = SEND_NO_ACKNOWLEDGE;
                            next_state = STOP;
                        }
                    }
                }
                    break;
                case STOP:
                    pin_i2c_write(PIN_I2C_SDA, 1); //create stop condition
                    i2c_master_state = IDLE;
                    break;
                default:
                    break;
            }
        }
    }
    timer_cycle = (timer_cycle + 1) % 4;
}


//creates master interface
void ICACHE_FLASH_ATTR i2c_master_init() {
    pin_i2c_write(PIN_I2C_SCL, 1);
    pin_i2c_write(PIN_I2C_SDA, 1);
}

//reads from slave
void i2c_master_read(int length) {
    receive_counter += length;
}

//writes to slave
void ICACHE_FLASH_ATTR i2c_master_write(const uint8 *data) {
    // os_printf("i2c_master_write: %s\n", data);
    ring_buffer_write(&i2c_master_send_buffer, data);
}

//writes to slave
void ICACHE_FLASH_ATTR i2c_master_write_byte(const uint8 data) {
    // os_printf("i2c_master_write_byte: %d %c\n", data, data);
    ring_buffer_write_one_byte(&i2c_master_send_buffer, data);
}

//sets the address where messages will be send to (Default address: 0000000)
void ICACHE_FLASH_ATTR i2c_master_set_target_address(int addresscode) {
    address = addresscode;
}
