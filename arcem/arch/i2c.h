/* (c) David Alan Gilbert 1995 - see Readme file for copying info */
#ifndef I2C_HEADER
#define I2C_HEADER

/*------------------------------------------------------------------------------*/
/* Called when the I2C clock changes state                                      */
void I2C_Update(ARMul_State *state);

/* ------------------------------------------------------------------------- */
bool I2C_Init(ARMul_State *state);

/* A front end may hand the CMOS RAM over in memory, as the chip's 256 bytes
   (NULL for all zeros), and then no hexcmos file is read or written. Both are
   set before the emulator starts. */
#include <time.h>
extern bool I2C_CMOSInMemory;
extern const uint8_t *I2C_CMOSData;

/* A front end may supply the clock the RTC reads. NULL means the host's. */
extern time_t (*I2C_HostTime)(void);

/* The chip's 256 bytes as they are now */
void I2C_GetCMOS(uint8_t *out);

#endif
