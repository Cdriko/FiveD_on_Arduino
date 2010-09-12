#include	"serial.h"

#include	"arduino.h"

#include	<avr/interrupt.h>

#define		BUFSIZE			128
#define		BAUD			115200

#define		ASCII_XOFF	19
#define		ASCII_XON		17

volatile uint8_t rxhead = 0;
volatile uint8_t rxtail = 0;
volatile uint8_t rxbuf[BUFSIZE];

volatile uint8_t txhead = 0;
volatile uint8_t txtail = 0;
volatile uint8_t txbuf[BUFSIZE];

#define	buf_canread(buffer)			((buffer ## head - buffer ## tail    ) & (BUFSIZE - 1))
#define	buf_canwrite(buffer)		((buffer ## tail - buffer ## head - 1) & (BUFSIZE - 1))

#define	buf_push(buffer, data)	do { buffer ## buf[buffer ## head] = data; buffer ## head = (buffer ## head + 1) & (BUFSIZE - 1); } while (0)
#define	buf_pop(buffer, data)		do { data = buffer ## buf[buffer ## tail]; buffer ## tail = (buffer ## tail + 1) & (BUFSIZE - 1); } while (0)

/*
	ringbuffer logic:
	head = written data pointer
	tail = read data pointer

	when head == tail, buffer is empty
	when head + 1 == tail, buffer is full
	thus, number of available spaces in buffer is (tail - head) & bufsize

	can write:
	(tail - head - 1) & (BUFSIZE - 1)

	write to buffer:
	buf[head++] = data; head &= (BUFSIZE - 1);

	can read:
	(head - tail) & (BUFSIZE - 1)

	read from buffer:
	data = buf[tail++]; tail &= (BUFSIZE - 1);
*/

volatile uint8_t flowflags = 0;
#define		FLOWFLAG_SEND_XOFF	1
#define		FLOWFLAG_SEND_XON		2
#define		FLOWFLAG_SENT_XOFF	4
#define		FLOWFLAG_SENT_XON		8

void serial_init()
{
#if BAUD > 38401
	UCSR0A = MASK(U2X0);
#else
	UCSR0A = 0;
#endif
#if BAUD > 38401
	UBRR0 = (((F_CPU / 8) / BAUD) - 0.5);
#else
	UBRR0 = (((F_CPU / 16) / BAUD) - 0.5);
#endif

	UCSR0B = MASK(RXEN0) | MASK(TXEN0);
	UCSR0C = MASK(UCSZ01) | MASK(UCSZ00);

	UCSR0B |= MASK(RXCIE0) | MASK(UDRIE0);
}

/*
	Interrupts, UART 0 for mendel
*/

ISR(USART0_RX_vect)
{
	if (buf_canwrite(rx))
		buf_push(rx, UDR0);
}

ISR(USART0_UDRE_vect)
{
	#if XONXOFF
	if (flowflags & FLOWFLAG_SEND_XOFF) {
		UDR0 = ASCII_XOFF;
		flowflags = (flowflags & ~FLOWFLAG_SEND_XOFF) | FLOWFLAG_SENT_XOFF;
	}
	else if (flowflags & FLOWFLAG_SEND_XON) {
		UDR0 = ASCII_XON;
		flowflags = (flowflags & ~FLOWFLAG_SEND_XON) | FLOWFLAG_SENT_XON;
	}
	else
	#endif
	if (buf_canread(tx))
		buf_pop(tx, UDR0);
	else
		UCSR0B &= ~MASK(UDRIE0);
}

/*
	Read
*/

uint8_t serial_rxchars()
{
	return buf_canread(rx);
}

uint8_t serial_popchar()
{
	uint8_t c = 0;
	// it's imperative that we check, because if the buffer is empty and we pop, we'll go through the whole buffer again
	if (buf_canread(rx))
		buf_pop(rx, c);
	return c;
}

/*
	Write
*/

// uint8_t serial_txchars()
// {
// 	return buf_canwrite(tx);
// }

void serial_writechar(uint8_t data)
{
	// check if interrupts are enabled
	if (SREG & MASK(SREG_I)) {
		// if they are, we should be ok to block since the tx buffer is emptied from an interrupt
		for (;buf_canwrite(tx) == 0;);
		buf_push(tx, data);
	}
	else {
		// interrupts are disabled- maybe we're in one?
		// anyway, instead of blocking, only write if we have room
		if (buf_canwrite(tx))
			buf_push(tx, data);
	}
	// enable TX interrupt so we can send this character
	UCSR0B |= MASK(UDRIE0);
}

void serial_writeblock(void *data, int datalen)
{
	int i;

	for (i = 0; i < datalen; i++)
		serial_writechar(((uint8_t *) data)[i]);
}

void serial_writestr(uint8_t *data)
{
	uint8_t i = 0, r;
	// yes, this is *supposed* to be assignment rather than comparison, so we break when r is assigned zero
// 	for (uint8_t r; (r = data[i]); i++)
	while ((r = data[i++]))
		serial_writechar(r);
}

/*
	Write from FLASH

	Extensions to output flash memory pointers. This prevents the data to
	become part of the .data segment instead of the .code segment. That means
	less memory is consumed for multi-character writes.

	For single character writes (i.e. '\n' instead of "\n"), using
	serial_writechar() directly is the better choice.
*/

void serial_writeblock_P(PGM_P data, int datalen)
{
	int i;

	for (i = 0; i < datalen; i++)
		serial_writechar(pgm_read_byte(&data[i]));
}

void serial_writestr_P(PGM_P data)
{
	uint8_t r, i = 0;
	// yes, this is *supposed* to be assignment rather than comparison, so we break when r is assigned zero
	for ( ; (r = pgm_read_byte(&data[i])); i++)
		serial_writechar(r);
}

#ifdef	XONXOFF
	void xon() {
		if (flowflags & FLOWFLAG_SENT_XOFF)
			flowflags = FLOWFLAG_SEND_XON;
		// enable TX interrupt so we can send this character
		UCSR0B |= MASK(UDRIE0);
	}

	void xoff() {
		flowflags = FLOWFLAG_SEND_XOFF;
		// enable TX interrupt so we can send this character
		UCSR0B |= MASK(UDRIE0);
	}
#endif
