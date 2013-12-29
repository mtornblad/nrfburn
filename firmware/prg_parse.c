#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <avr/io.h>

#include "hw_setup.h"
#include "avr_serial.h"
#include "utils.h"
#include "prg_parse.h"
#include "prg_spi.h"

// request buffer
union {
	req_simple_t		simple;
	req_read_flash_t	readFlash;
	req_write_flash_t	writeFlash;
	req_erase_page_t	erasePage;

	uint8_t		raw[MAX_PROT_BUFF_SIZE];
} reqBuffer;

// response buffer
union {
	resp_simple_t			simple;
	resp_error_t			error;
	resp_version_t			ver;
	resp_read_flash_t		readFlash;
	resp_read_fsr_fpcr_t	readFsrFpcr;
	
	uint8_t		raw[MAX_PROT_BUFF_SIZE];
} respBuffer;

uint8_t reqPos;				// index of the next character in reqBuffer
bool reqComplete = false;	// true if we have an entire request in reqBuffer

uint8_t txPos = 0, txLen = 0;

void progParseInit(void)
{
	/*
	// setup the timer used for communication timeout detection
	TCCR1B = _BV(CS12);		// prescaler 256, TCNT=16us, OVF=1.048576s
	*/
}

/*
static inline bool prgTimerOverflow(void)
{
	return TIFR1 & _BV(TOV1);
}

static inline void prgTimerReset(void)
{
	TCNT1 = 0xffff - 12500;		// 200ms
	TIFR1 = _BV(TOV1);	// reset by writing one to TOV1
}
*/

void progParseResetRx(void)
{
	reqPos = 0;
}

// calcs the checksum of the message in the response buffer.
// assumes the entire response is valid except for the checksum.
// sets txPos and txLen which send the response on it's way
static void prgPrepareResponse(void)
{
	uint8_t sum = 0;
	uint8_t cnt = respBuffer.simple.length - 1;
	uint8_t* p = respBuffer.raw;
	while (cnt)
	{
		sum ^= *p++;
		cnt--;
	}

	*p = sum;
	
	txPos = 0;
	txLen = respBuffer.simple.length;
}

void progParseSetRxByte(uint8_t byte)
{
	/* if (prgTimerOverflow())
		progParseResetRx(); */

	if (reqPos < MAX_PROT_BUFF_SIZE  &&  !reqComplete)
	{
		reqBuffer.raw[reqPos++] = byte;

		if (reqPos > 1)	// do we have the message length?
		{
			if (reqBuffer.simple.length > MAX_PROT_BUFF_SIZE)
			{
				progParseResetRx();
			} else if (reqPos == reqBuffer.simple.length) {	// message complete?

				// checksum
                uint8_t sum = 0;
                uint8_t* p = reqBuffer.raw;
                while (reqPos)
				{
                    sum ^= *p++;
                    reqPos--;
                }

				if (sum == 0)
				{
					reqComplete = true;
				} else {	// checksum error
					SetBit(PORT(LED2_PORT), LED2_BIT);
					SetBit(PORT(LED1_PORT), LED1_BIT);
				
					respBuffer.error.length = sizeof respBuffer.error;
					respBuffer.error.response = respError;
					respBuffer.error.error_code = respErrBadChecksum;

					prgPrepareResponse();
				}
			}
		}
	} else {
		progParseResetRx();
	}

	// prgTimerReset();
}

int progParseGetTxByte(void)
{
	if (txPos == txLen)
	{
		txPos = txLen = 0;
		return -1;
	}
		
	return respBuffer.raw[txPos++];
}

int progParseGetTxCount(void)
{
	return txLen - txPos;
}

static void readFlash(void)
{
	respBuffer.readFlash.length = sizeof respBuffer.readFlash;

	ProgSpiREAD(reqBuffer.readFlash.address, respBuffer.readFlash.data);
}

static void setTimeoutResponse(void)
{
	respBuffer.error.length = sizeof respBuffer.error;
	respBuffer.error.response = respError;
	respBuffer.error.error_code = respErrTimeoutExpired;
}

static void writeFlash(void)
{
	// set WEN and INFEN
	uint8_t fsr = (ProgSpiRDSR() & _BV(FSR_DBG)) | _BV(FSR_WEN);
	if (reqBuffer.writeFlash.request == reqWriteMainBlock)
		ProgSpiWRSR(fsr);
	else
		ProgSpiWRSR(fsr | _BV(FSR_INFEN));
	
	// do the programming of PROG_CHUNK_SIZE bytes
	ProgSpiPROGRAM(reqBuffer.writeFlash.address, reqBuffer.writeFlash.data);
	
	// wait for confirmation
	if (!ProgSpiWaitForRDYN(PROGRAM_CHUNK_TIMEOUT))
		setTimeoutResponse();
}

static void erasePage(void)
{
	// set WEN and INFEN
	uint8_t fsr = (ProgSpiRDSR() & _BV(FSR_DBG)) | _BV(FSR_WEN);
	if (reqBuffer.erasePage.request == reqErasePageMB)
		ProgSpiWRSR(fsr);
	else
		ProgSpiWRSR(fsr | _BV(FSR_INFEN));

	ProgSpiERASE_PAGE(reqBuffer.erasePage.page_num);
	if (!ProgSpiWaitForRDYN(ERASE_PAGE_TIMEOUT))
		setTimeoutResponse();
}

void prgValidate(void)
{
	// set the default (optimistic) response header
	respBuffer.simple.length = sizeof respBuffer.simple;
	respBuffer.simple.response = reqBuffer.simple.request | 0x80;
	
	switch (reqBuffer.simple.request)
	{
	case reqVersion:
		respBuffer.ver.length = sizeof respBuffer.ver;
		respBuffer.ver.ver_major = PRG_VER_MAJOR;
		respBuffer.ver.ver_minor = PRG_VER_MINOR;
		break;

	case reqProgBegin:
		ProgSpiBegin();
		break;

	case reqProgEnd:
		ProgSpiEnd();
		break;

	case reqReadFsrFpcr:
		respBuffer.readFsrFpcr.length = sizeof respBuffer.readFsrFpcr;
		respBuffer.readFsrFpcr.fsr = ProgSpiRDSR();
		respBuffer.readFsrFpcr.fpcr = ProgSpiRDFPCR();
		break;
		
	case reqReadMainBlock:
	case reqReadInfoPage:
		readFlash();
		break;

	case reqWriteMainBlock:
	case reqWriteInfoPage:
		writeFlash();
		break;
		
	case reqEraseAll:
		ProgSpiWREN();
		ProgSpiERASE_ALL();
		if (!ProgSpiWaitForRDYN(ERASE_ALL_TIMEOUT))
			setTimeoutResponse();
		break;
		
	case reqErasePageMB:
	case reqErasePageIP:
		erasePage();
		break;
		
	case reqDisReadMainBlock:
		ProgSpiWREN();
		ProgSpiRDISMB();
		if (!ProgSpiWaitForRDYN(1))
			setTimeoutResponse();
		break;
		
	case reqDisReadInfoPage:
		ProgSpiWREN();
		ProgSpiRDISIP();
		if (!ProgSpiWaitForRDYN(1))
			setTimeoutResponse();
		break;
	}

	prgPrepareResponse();
}

void progParsePoll()
{
	if (reqComplete)
	{
		reqComplete = false;
		prgValidate();
	}
}
