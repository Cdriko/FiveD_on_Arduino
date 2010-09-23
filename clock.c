/*
	clock.c

	a system clock with 1ms ticks
*/

#include	"clock.h"

#include	<avr/io.h>
#include	<avr/interrupt.h>

#include	"arduino.h"
#include	"pinout.h"

// global clock
#ifdef	GLOBAL_CLOCK
	volatile uint32_t	clock = 0;
#endif

uint8_t						clock_counter_10ms	= 0;
uint8_t						clock_counter_250ms	= 0;
uint8_t						clock_counter_1s		= 0;

volatile uint8_t	clock_flag					= 0;

void clock_setup() {
	// use system clock
	ASSR = 0;

	// no compare match, CTC mode
	TCCR2A = MASK(WGM21);

	// 128 prescaler (16MHz / 128 = 125KHz)
	TCCR2B = MASK(CS22) | MASK(CS20);

	// 125KHz / 125 = 1KHz for a 1ms tick rate
	OCR2A = (F_CPU / 128 / 1000);

	// interrupt on match, when counter reaches OCR2A
	TIMSK2 |= MASK(OCIE2A);
}

ISR(TIMER2_COMPA_vect) {
	// global clock
#ifdef	GLOBAL_CLOCK
	clock++;
#endif
	// 10ms tick
	if (++clock_counter_10ms == 10) {
		clock_flag |= CLOCK_FLAG_10MS;
		clock_counter_10ms = 0;
	}

	// 1/4 second tick
	if (++clock_counter_250ms == 250) {
		clock_flag |= CLOCK_FLAG_250MS;
		clock_counter_250ms = 0;
		if (++clock_counter_1s == 4) {
			clock_flag |= CLOCK_FLAG_1S;
			clock_counter_1s = 0;
		}
	}
}

#ifdef	GLOBAL_CLOCK
uint32_t clock_read() {
	uint32_t	c;

	cli();			// set atomic
	c = clock;	// copy clock value
	sei();			// release atomic

	return c;
}
#endif
