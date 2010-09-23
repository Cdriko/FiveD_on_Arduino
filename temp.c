#include	"temp.h"

#include	<stdlib.h>
#include	<avr/eeprom.h>
#include	<avr/pgmspace.h>

#include	"arduino.h"
#include	"timer.h"
#include	"machine.h"
#include	"debug.h"
#include	"sersendf.h"
#include	"heater.h"

typedef enum {
	TT_THERMISTOR,
	TT_MAX6675,
	TT_AD595
} temp_types;

typedef enum {
	PRESENT,
	TCOPEN
} temp_flags_enum;

/***************************************************************************\
*                                                                           *
* Fill in the following two structs according to your hardware              *
*                                                                           *
* If your temperature sensor has no associated heater, enter '255' as the   *
*   heater index.                                                           *
*                                                                           *
\***************************************************************************/
struct {
	uint8_t						temp_type;
	uint8_t						temp_pin;
	
	uint8_t						heater_index;
} temp_sensors[NUM_TEMP_SENSORS] =
{
	{
		TT_MAX6675,
		0,
		0
	}
};

/***************************************************************************\
*                                                                           *
* End                                                                       *
*                                                                           *
\***************************************************************************/

// this struct holds the runtime sensor data- read temperatures, targets, etc
struct {
	temp_flags_enum		temp_flags;
	
	uint16_t					last_read_temp;
	uint16_t					target_temp;
	
	uint8_t						temp_residency;
	
	uint16_t					next_read_time;
} temp_sensors_runtime[NUM_TEMP_SENSORS];

#ifdef	TEMP_MAX6675
#endif

#ifdef	TEMP_THERMISTOR
#include	"analog.h"

#define NUMTEMPS 20
uint16_t temptable[NUMTEMPS][2] PROGMEM = {
	{1, 841},
	{54, 255},
	{107, 209},
	{160, 184},
	{213, 166},
	{266, 153},
	{319, 142},
	{372, 132},
	{425, 124},
	{478, 116},
	{531, 108},
	{584, 101},
	{637, 93},
	{690, 86},
	{743, 78},
	{796, 70},
	{849, 61},
	{902, 50},
	{955, 34},
	{1008, 3}
};
#endif

#ifdef	TEMP_AD595
#include	"analog.h"
#endif

void temp_sensor_tick() {
	uint8_t	i = 0, all_within_range = 1;
	for (; i < NUM_TEMP_SENSORS; i++) {
		if (temp_sensors_runtime[i].next_read_time) {
			temp_sensors_runtime[i].next_read_time--;
		}
		else {
			uint16_t	temp = 0;
			#ifdef	TEMP_THERMISTOR
			uint8_t		j;
			#endif
			//time to deal with this temp/heater
			switch(temp_sensors[i].temp_type) {
				#ifdef	TEMP_MAX6675
				case TT_MAX6675:
					#ifdef	PRR
					PRR &= ~MASK(PRSPI);
					#elif defined PRR0
					PRR0 &= ~MASK(PRSPI);
					#endif
					
					SPCR = MASK(MSTR) | MASK(SPE) | MASK(SPR0);
					
					// enable TT_MAX6675
					WRITE(SS, 0);
					
					// ensure 100ns delay - a bit extra is fine
					delay(1);
					
					// read MSB
					SPDR = 0;
					for (;(SPSR & MASK(SPIF)) == 0;);
					temp = SPDR;
					temp <<= 8;
					
					// read LSB
					SPDR = 0;
					for (;(SPSR & MASK(SPIF)) == 0;);
					temp |= SPDR;
					
					// disable TT_MAX6675
					WRITE(SS, 1);
					
					temp_sensors_runtime[i].temp_flags = 0;
					if ((temp & 0x8002) == 0) {
						// got "device id"
						temp_sensors_runtime[i].temp_flags |= PRESENT;
						if (temp & 4) {
							// thermocouple open
							temp_sensors_runtime[i].temp_flags |= TCOPEN;
						}
						else {
							temp = temp >> 3;
						}
					}
					
					// FIXME: placeholder number
					temp_sensors_runtime[i].next_read_time = 25;
					
					break;
				#endif	/* TEMP_MAX6675	*/

				#ifdef	TEMP_THERMISTOR
				case TT_THERMISTOR:
					
					//Read current temperature
					temp = analog_read(temp_sensors[i].temp_pin);
					
					//Calculate real temperature based on lookup table
					for (j = 1; j < NUMTEMPS; j++) {
						if (pgm_read_word(&(temptable[j][0])) > temp) {
							// multiply by 4 because internal temp is stored as 14.2 fixed point
							temp = pgm_read_word(&(temptable[j][1])) + (pgm_read_word(&(temptable[j][0])) - temp) * 4 * (pgm_read_word(&(temptable[j-1][1])) - pgm_read_word(&(temptable[j][1]))) / (pgm_read_word(&(temptable[j][0])) - pgm_read_word(&(temptable[j-1][0])));
							break;
						}
					}
					
					//Clamp for overflows
					if (j == NUMTEMPS)
						temp = temptable[NUMTEMPS-1][1];
					
					// FIXME: placeholder number
					temp_sensors_runtime[i].next_read_time = 0;
					
					break;
				#endif	/* TEMP_THERMISTOR */

				#ifdef	TEMP_AD595
				case TT_AD595:
					temp = analog_read(temp_pin);
					
					// convert
					// >>8 instead of >>10 because internal temp is stored as 14.2 fixed point
					temp = (temp * 500L) >> 8;
					
					// FIXME: placeholder number
					temp_sensors[i].next_read_time = 0;
					
					break;
				#endif	/* TEMP_AD595 */
			}
			temp_sensors_runtime[i].last_read_temp = temp;
			
			if (labs(temp - temp_sensors_runtime[i].target_temp) < TEMP_HYSTERESIS) {
				if (temp_sensors_runtime[i].temp_residency < TEMP_RESIDENCY_TIME)
					temp_sensors_runtime[i].temp_residency++;
			}
			else {
				temp_sensors_runtime[i].temp_residency = 0;
				all_within_range = 0;
			}
			
			if (temp_sensors[i].heater_index != 255) {
				heater_tick(temp_sensors[i].heater_index, temp_sensors_runtime[i].last_read_temp, temp_sensors_runtime[i].target_temp);
			}
		}
	}
}

uint8_t	temp_achieved() {
	uint8_t i, all_ok = 255;
	for (i = 0; i < NUM_TEMP_SENSORS; i++) {
		if (temp_sensors_runtime[i].temp_residency < TEMP_RESIDENCY_TIME)
			all_ok = 0;
	}
	return all_ok;
}

void temp_set(uint8_t index, uint16_t temperature) {
	temp_sensors_runtime[index].target_temp = temperature;
	temp_sensors_runtime[index].temp_residency = 0;
}

void temp_print(uint8_t index) {
	uint8_t c = 0;
	
	c = (temp_sensors_runtime[index].last_read_temp & 3) * 25;
	
	sersendf_P(PSTR("T: %u.%u\n"), temp_sensors_runtime[index].last_read_temp >> 2, c);
}
