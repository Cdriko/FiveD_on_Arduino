#include	"heater.h"

#include	<avr/eeprom.h>
#include	<avr/pgmspace.h>

#include	"arduino.h"
#include	"timer.h"
#include	"machine.h"
#include	"debug.h"
#include	"sersendf.h"

/***************************************************************************\
*                                                                           *
* Fill in the following two structs according to your hardware              *
*                                                                           *
\***************************************************************************/
struct {
	volatile uint8_t	*heater_port;
	uint8_t						heater_pin;
	volatile uint8_t	*heater_pwm;
} heaters[NUM_HEATERS] =
{
	{
		&PORTD,
		PIND0,
		&OCR0A
	}
};

/***************************************************************************\
*                                                                           *
* End                                                                       *
*                                                                           *
\***************************************************************************/

// this struct holds the heater PID factors that are stored in the EEPROM during poweroff
struct {
	int32_t						p_factor;
	int32_t						i_factor;
	int32_t						d_factor;
	int16_t						i_limit;
} heaters_pid[NUM_HEATERS];

// this struct holds the runtime heater data- PID counters and such
struct {
	int16_t						heater_p;
	int16_t						heater_i;
	int16_t						heater_d;
	
	uint8_t						pid_output;

	uint16_t					temp_history[TH_COUNT];
	uint8_t						temp_history_pointer;
} heaters_runtime[NUM_HEATERS];

#define		DEFAULT_P				8192
#define		DEFAULT_I				512
#define		DEFAULT_D				-24576
#define		DEFAULT_I_LIMIT	384

typedef struct {
	int32_t		EE_p_factor;
	int32_t		EE_i_factor;
	int32_t		EE_d_factor;
	int16_t		EE_i_limit;
} EE_factor;

EE_factor EEMEM EE_factors[NUM_HEATERS];

void heater_init() {
	// read factors from eeprom
	uint8_t i;
	for (i = 0; i < NUM_HEATERS; i++) {
		heaters_pid[i].p_factor = eeprom_read_dword((uint32_t *) &EE_factors[i].EE_p_factor);
		heaters_pid[i].i_factor = eeprom_read_dword((uint32_t *) &EE_factors[i].EE_i_factor);
		heaters_pid[i].d_factor = eeprom_read_dword((uint32_t *) &EE_factors[i].EE_d_factor);
		heaters_pid[i].i_limit = eeprom_read_word((uint16_t *) &EE_factors[i].EE_i_limit);
		
		if ((heaters_pid[i].p_factor == 0) && (heaters_pid[i].i_factor == 0) && (heaters_pid[i].d_factor == 0) && (heaters_pid[i].i_limit == 0)) {
			heaters_pid[i].p_factor = DEFAULT_P;
			heaters_pid[i].i_factor = DEFAULT_I;
			heaters_pid[i].d_factor = DEFAULT_D;
			heaters_pid[i].i_limit = DEFAULT_I_LIMIT;
		}
	}
}

void heater_save_settings() {
	uint8_t i;
	for (i = 0; i < NUM_HEATERS; i++) {
		eeprom_write_dword((uint32_t *) &EE_factors[i].EE_p_factor, heaters_pid[i].p_factor);
		eeprom_write_dword((uint32_t *) &EE_factors[i].EE_i_factor, heaters_pid[i].i_factor);
		eeprom_write_dword((uint32_t *) &EE_factors[i].EE_d_factor, heaters_pid[i].d_factor);
		eeprom_write_word((uint16_t *) &EE_factors[i].EE_i_limit, heaters_pid[i].i_limit);
	}
}

void heater_tick(uint8_t h, uint16_t current_temp, uint16_t target_temp) {
	// now for heater stuff
	int16_t	t_error = target_temp - current_temp;

	heaters_runtime[h].temp_history[heaters_runtime[h].temp_history_pointer++] = current_temp;
	heaters_runtime[h].temp_history_pointer &= (TH_COUNT - 1);

	// PID stuff
	// proportional
	heaters_runtime[h].heater_p = t_error;

	// integral
	heaters_runtime[h].heater_i += t_error;
	// prevent integrator wind-up
	if (heaters_runtime[h].heater_i > heaters_pid[h].i_limit)
		heaters_runtime[h].heater_i = heaters_pid[h].i_limit;
	else if (heaters_runtime[h].heater_i < -heaters_pid[h].i_limit)
		heaters_runtime[h].heater_i = -heaters_pid[h].i_limit;

	// derivative
	// note: D follows temp rather than error so there's no large derivative when the target changes
	heaters_runtime[h].heater_d = current_temp - heaters_runtime[h].temp_history[heaters_runtime[h].temp_history_pointer];

	// combine factors
	int32_t pid_output_intermed = (
		(
			(((int32_t) heaters_runtime[h].heater_p) * heaters_pid[h].p_factor) +
			(((int32_t) heaters_runtime[h].heater_i) * heaters_pid[h].i_factor) +
			(((int32_t) heaters_runtime[h].heater_d) * heaters_pid[h].d_factor)
		) / PID_SCALE
	);

	// rebase and limit factors
	if (pid_output_intermed > 255)
		heaters_runtime[h].pid_output = 255;
	else if (pid_output_intermed < 0)
		heaters_runtime[h].pid_output = 0;
	else
		heaters_runtime[h].pid_output = pid_output_intermed & 0xFF;

	if (debug_flags & DEBUG_PID)
		sersendf_P(PSTR("T{E:%d, P:%d * %ld = %ld / I:%d * %ld = %ld / D:%d * %ld = %ld # O: %ld = %u}\n"), t_error, heaters_runtime[h].heater_p, heaters_pid[h].p_factor, (int32_t) heaters_runtime[h].heater_p * heaters_pid[h].p_factor / PID_SCALE, heaters_runtime[h].heater_i, heaters_pid[h].i_factor, (int32_t) heaters_runtime[h].heater_i * heaters_pid[h].i_factor / PID_SCALE, heaters_runtime[h].heater_d, heaters_pid[h].d_factor, (int32_t) heaters_runtime[h].heater_d * heaters_pid[h].d_factor / PID_SCALE, pid_output_intermed, heaters_runtime[h].pid_output);

	heater_set(h, heaters_runtime[h].pid_output);
}

void heater_set(uint8_t index, uint8_t value) {
	if (heaters[index].heater_pwm) {
		*heaters[index].heater_pwm = value;
	}
	else {
		if (value >= 8)
			*heaters[index].heater_port |= MASK(heaters[index].heater_pin);
		else
			*heaters[index].heater_port &= ~MASK(heaters[index].heater_pin);
	}
}

void pid_set_p(uint8_t index, int32_t p) {
	heaters_pid[index].p_factor = p;
}

void pid_set_i(uint8_t index, int32_t i) {
	heaters_pid[index].i_factor = i;
}

void pid_set_d(uint8_t index, int32_t d) {
	heaters_pid[index].d_factor = d;
}

void pid_set_i_limit(uint8_t index, int32_t i_limit) {
	heaters_pid[index].i_limit = i_limit;
}
