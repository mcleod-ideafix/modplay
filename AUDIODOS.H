#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <mem.h>
#include <dos.h>

#define SB_RESET         0x06       // DSP - Reset                             Write       SB
#define SB_READ_DATA     0x0A       // DSP - Read Data                         Read        SB
#define SB_WRITE_DATCOM  0x0C       // DSP - Write Data or Command             Write       SB
#define SB_BUFSTATUS     0x0C       // DSP - Write Buffer Status               Read        SB
#define SB_TIMINTCLEAR   0x0D       // DSP - Timer Interrupt Clear             Read        SB16???
#define SB_DATA_AVAIL    0x0E       // DSP - Data Available Status             Read        SB
#define SB_IRQACK        0x0E       // DSP - IRQ Acknowledge, 8-bit            Read        SB
#define SB_IRQACK16      0x0F       // DSP - IRQ Acknowledge, 16-bit           Read        SB16

#ifndef MAXAUDIOBUFFERS
#define MAXAUDIOBUFFERS 4
#endif

// This is to hold all information about a block of memory
// for the first physical MByte, which is in turn needed
// so the DMA in the ISA bus can work with it.
typedef struct
{
  uint16_t rmsegment;
  uint16_t pmselector;
  uint8_t *p;
} DPMIDosMem;

// Description of a sound buffer. This aims to mimic the
// behaviour of Win32 buffers for digital audio devices
typedef struct
{
  uint8_t *p;
  size_t lbuf;
  int enuso;
  int preparado;
  int terminado;
} TBuffer;

volatile TBuffer sbuf[MAXAUDIOBUFFERS];  // buffers for continuous audio streaming
volatile int current_read_buffer;  // which buffer am I reading from right now
volatile int current_write_buffer; // which buffer am I writting to right now
volatile int audio_parado;  // switch that indicates that no audio buffers are ready and hence, audio is stopped

// Sound Blaster params
static uint16_t io;
static uint8_t irq, dma;
static uint32_t sampling_frequency;

// The DMA block
static DPMIDosMem bloque;

// User function to be called from the interrupt function, to fill the next buffer
typedef void (*TFuncionCBUsuario)(void);

// handler and previous handler for the Sound Blaster IRQ
void __interrupt SBIsr(void);
void (__interrupt * Old_SBIsr)(void);

TFuncionCBUsuario pfucb = NULL;  // default user function is none

// Function: allocates memory from the so called "DOS memory" (first megabyte)
// by using DOS functions.
int DosMalloc (size_t mem, DPMIDosMem *data)
{
  union REGPACK regs;

  memset( &regs, 0, sizeof regs);
  regs.w.ax = 0x100; /* DPMI alloc from DOS mem */
  regs.w.bx = (mem/16)+(mem%16 != 0);
  intr( 0x31, &regs );

  data->rmsegment = regs.w.ax;
  data->pmselector = regs.w.dx;

  memset( &regs, 0, sizeof regs );
  regs.w.bx = data->pmselector;
  regs.w.ax = 0x0006;
  intr (0x31, &regs);

  data->p = (void *) (regs.w.cx << 16 | regs.w.dx);
  return 1;
}

// Function: frees DOS memory
int DosFree (DPMIDosMem *data)
{
  union REGPACK regs;

  memset( &regs, 0, sizeof regs);
  regs.w.ax = 0x101; /* DPMI free DOS mem */
  regs.w.dx = data->pmselector;
  intr( 0x31, &regs );
  return 1;
}

// Function: retrieves Sound Blaster params from BLASTER variable
int SBParams (void)
{
  char *env;
  size_t i;

  env = getenv("BLASTER");
  if (!env)
    return 0;

  i = 0;
  io = 0;
  irq = 0;
  dma = 0;
  while (i<strlen(env))
  {
    if (env[i] == 'A')
    {
      i++;
      while (isdigit(env[i]))
      {
        io = io * 16 + (env[i] - '0');
        i++;
      }
    }
    else if (env[i] == 'I')
    {
      i++;
      while (isdigit(env[i]))
      {
        irq = irq * 10 + (env[i] - '0');
        i++;
      }
    }
    else if (env[i] == 'D')
    {
      i++;
      while (isdigit(env[i]))
      {
        dma = dma * 10 + (env[i] - '0');
        i++;
      }
    }
    else
      i++;
  }
  return 1;
}

// Function: Reset and init DSP
int SBReset (void)
{
  size_t tout;

  outp (io+SB_RESET,1);
  delay(1);
  outp (io+SB_RESET,0);
  delay(1);
  for (tout=0; tout<65536; tout++)
    if (inp (io+SB_DATA_AVAIL) & 0x80)
      if (inp (io+SB_READ_DATA) == 0xAA)
        break;
  if (tout == 65536)
    return 0;

  return 1;
}

// Function: writes a command or data to DSP
void WriteDSP (uint8_t v)
{
  while (inp(io+SB_BUFSTATUS) & 0x80) { }
  outp (io+SB_WRITE_DATCOM, v);
}

// Function: reads data from DSP
uint8_t ReadDSP (void)
{
  while (!(inp(io+SB_DATA_AVAIL) & 0x80)) { }
  return inp(io+SB_READ_DATA);
}

// Function: plays digitized 8-bit audio using DMA. This function uses some
// global variables, mostly those that keep Sound Blaster params read from
// BLASTER environment variable
void SBPlayDMA (uint8_t *buffer, uint16_t lbuff)
{
  uint16_t dmapageport[] = {0x87, 0x83, 0x81, 0x82};   // High byte address page for DMA channels 0 to 3
  uint32_t addr = (uint32_t)(buffer);

  // First, DMA is programmed to know which type of transfer to do, where samples are
  // and how many of them are.
  lbuff--;
  outp (0xA, dma | 0x04);   // mask DMA channel
  outp (0xC, 0);            // reset flipflop lsb/msb
  outp (0xB, dma | 0x48);   // Single transfer, no auto restart
  outp (dmapageport[dma], (addr>>16) & 0xFF);  // set DMA page (address bit 16-24)
  outp (dma*2, addr & 0xFF);         // Physical memory
  outp (dma*2, (addr>>8) & 0xFF);    // low 16 bit address
  outp (dma*2+1, lbuff & 0xFF);         // bytes to transfer
  outp (dma*2+1, (lbuff>>8) & 0xFF);    // less 1
  outp (0xA, dma);          // unmask DMA channel

  // DMA is ready to accept DRQ requests. Now we program the DSP to generate such DRQ
  // request at the desired sampling frequency. DSP does not know anything about where
  // the samples are stored, but must know how many of them are going to be played, and
  // ant which rate.
  WriteDSP (0x40); // command to set sampling frequency
  WriteDSP (256 - 1000000L/sampling_frequency); // sampling frequency
  WriteDSP (0x48);   // command to set length of transfer
  WriteDSP (lbuff & 0xFF);         // LSB and MSB for transfer length
  WriteDSP ((lbuff>>8) & 0xFF);    // less 1 (as in DMA)
  WriteDSP (0x91);  // trigger DMA play (single, 8 bits, high speed, no restart)
}

// IRQ handler
void __interrupt SBIsr(void)
{
  inp (io + SB_IRQACK);   // ACK to DSP
  outp (0x20, 0x20);      // EOI to 8259
  if (irq>=8)
    outp (0xA0, 0x20);    // EOI to slave 8259 if needed

  sbuf[current_read_buffer].enuso = 0; // current read buffer is marked as not in use
  sbuf[current_read_buffer].terminado = 1;  // and finished

  current_read_buffer = (current_read_buffer + 1)%MAXAUDIOBUFFERS;  // move to the next buffer
  if (sbuf[current_read_buffer].preparado != 1)   // if it is not ready,
    audio_parado = 1;                             // stop audio
  else
  {
    sbuf[current_read_buffer].enuso = 1;     // else mark this new buffer as in use
    sbuf[current_read_buffer].preparado = 0; // and not prepared
    SBPlayDMA (sbuf[current_read_buffer].p, sbuf[current_read_buffer].lbuf);  // and go playing it
  }
  if (pfucb)   // if there is a user function to call
    pfucb();   // jump to it
}

// Function: initialize memory and Sound Blaster system
int AbrirAudioCallBack (uint32_t sfreq, TFuncionCBUsuario p)
{
  uint8_t dsp_major, dsp_minor;
  int i;

  pfucb = p;
  sampling_frequency = sfreq;

  DosMalloc (131072, &bloque);   // get 128K of DOS memory
  // we need a block with double the size we actually need because we are seeking for an
  // address whose least 16 bits are zero, so we are at the beginning of a 64K page
  while (((uint32_t)(bloque.p) & 0xFFFF) != 0)
    bloque.p += 16;

  if (SBParams() == 0)
    return -1;

  // sorry! Only accept DMA channels 1 and 3 actually.
  if (dma>3)
    return -2;

  if (SBReset() == 0)  // if DSP not found, return (you may need to run CTCM.EXE to initialize PNP cards)
    return -3;

  WriteDSP (0xE1);  // identify DSP version
  dsp_major = ReadDSP();
  dsp_minor = ReadDSP();
  if (dsp_major < 2)  // we need DSP major version >= 2 to be able to use high speed command
    return -4;

  outp (0x20,0x20);   // EOI to master 8259
  outp (0xA0,0x20);   // and slave, just in case

  // Install a new IRQ handler to the Sound Blaster IRQ
  // and enable 8259 to accept INTs from that IRQ
  if (irq>=8)
  {
    Old_SBIsr = _dos_getvect(0x70 + irq-8);
    _dos_setvect(0x70 + irq-8, SBIsr);
    outp (0x21, inp(0x21) & 0xFB);
    outp (0xA1, inp(0xA1) & ~(1<<(irq-8)));
  }
  else
  {
    Old_SBIsr = _dos_getvect(8 + irq);
    _dos_setvect(8 + irq, SBIsr);
    outp (0x21, inp(0x21) & ~(1<<irq));
  }

  // The big 64K block is divided into 4K blocks
  // So DON'T ever set MAXAUDIOBUFFERS to a value greater than 16.
  for (i=0; i<MAXAUDIOBUFFERS; i++)
  {
    sbuf[i].p = bloque.p + 4096*i;
    sbuf[i].lbuf = 1;
    sbuf[i].enuso = 0;
    sbuf[i].preparado = 0;
    sbuf[i].terminado = 0;
    memset (sbuf[i].p, 128, 4096);
  }
  audio_parado = 1;
  current_read_buffer = 0;
  current_write_buffer = 0;
  WriteDSP (0xD1);   // enable speakers
  return 0;
}

// Function: opens audio device with no user function
int AbrirAudio (void)
{
  return AbrirAudioCallBack (44100, NULL);
}

// Functin: queue a block of audio samples to be played
void ReproducirAudio (uint8_t *data, int ldata)
{
  // If no buffers are currently playing...
  if (audio_parado)
  {
    audio_parado = 0;
    current_read_buffer = 0;     // resets indices to
    current_write_buffer = 0;    // write and read buffers
    sbuf[current_write_buffer].lbuf = ldata;             // populate current write buffer
    memcpy (sbuf[current_write_buffer].p, data, ldata);  // with data from the sample block
    sbuf[current_write_buffer].terminado = 0;  // mark it as not finished
    sbuf[current_write_buffer].preparado = 1;  // and ready to be played
    SBPlayDMA (sbuf[current_read_buffer].p, sbuf[current_read_buffer].lbuf);  // go and do it
    current_write_buffer = (current_write_buffer+1)%MAXAUDIOBUFFERS;  // buffer 0 is now playing. Buffer 1 is next to write to
  }
  else  // else, there is a buffer playing right now
  {
    while (current_read_buffer == current_write_buffer) { }  // ensures it is not the current read buffer
    sbuf[current_write_buffer].lbuf = ldata;     // and proceed with it as in the previous code block
    memcpy (sbuf[current_write_buffer].p, data, ldata);
    sbuf[current_write_buffer].terminado = 0;
    sbuf[current_write_buffer].preparado = 1;
    current_write_buffer = (current_write_buffer+1)%MAXAUDIOBUFFERS;
  }
}

// Function: closes audio device and free memory
void CerrarAudio (void)
{
  SBReset();  // resets DSP and exits from high speed mode
  WriteDSP (0xD3);   // silence speaker

  // restore old handlers and mask IRQ in 8259
  if (irq<8)
  {
    outp (0x21, inp(0x21) | (1<<irq));
    _dos_setvect(8 + irq, Old_SBIsr);
  }
  else
  {
    outp (0x21, inp(0x21) | 0x04);
    outp (0xA1, inp(0xA1) | (1<<(irq-8)));
    _dos_setvect(0x70 + irq-8, Old_SBIsr);
  }
  
  // disables user function
  pfucb = NULL;
  
  // frees DOS memory
  DosFree (&bloque);
}
