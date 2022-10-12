// MODPLAY. A small C portable Amiga Protracker module player
// Copyright (C) 2022 Miguel Angel Rodriguez Jodar (mcleod_ideafix).
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program.  If not, see <http://www.gnu.org/licenses/>.

#include <stdio.h>
#include <conio.h>
#include <stdlib.h>
#include <string.h>
#include <mem.h>
#include <stdint.h>
#include "audio.h"

// config option for player. It determines the master clock
// frequency that, in turn, is used to calculate the phase for the phase-accum
// counter
enum {PAL, NTSC};

// Sample information, as read from the MOD file
typedef struct
{
  char Samplename[23];
  size_t Samplelength;  // in bytes, not words as in the file
  uint8_t Finetune;     // originally 4 bits, signed, stored as unsigned.
  uint8_t Volume;
  size_t Repeatpoint;   // ditto as Samplelength
  size_t Repeatlength;  // ditto as Samplelength
  int8_t *Sampledata;
} TSample;

// Slot information. A slot is each of the 64 divisions in a pattern, for a
// given channel.
typedef struct
{
  uint8_t Samplenumber;
  uint16_t Noteperiod; // originally 12 bits, zero extended to 16 bits
  uint8_t Effect;
  uint8_t EffectArg;
  uint8_t NoteIndex;
  char Note[3];       // string for printing note pitch.
  uint8_t Octave;     // for printing note octave.
} TChannelData;

// A row (four slots) in a pattern.
typedef struct
{
  TChannelData chan[4];
} TRow;

// A pattern (64 rows)
typedef struct
{
  TRow row[64];
} TPattern;

// The while MOD info.
typedef struct
{
  char Songname[21];  // ASCIIZ string, padded with spaces
  TSample sample[31]; // up to 31 samples
  uint8_t Songpositions[128];  // up to 128 song positions
  uint8_t Songlength;  // how many actual song positions
  TPattern *pattern; // vector of patterns
  uint8_t Numpatterns;  // actual number of different patterns
} TModule;              // (taken from the highest value in Songpositions)

// Information about each audio channel we're playing
typedef struct
{
  size_t faseacum;     // phase-accummulator (mod 15) counter for that channel
  size_t fase;         // the phase for said counter
  uint16_t noteperiod; // current note period (Amiga format) we are playing
  uint8_t finetune;
  int volume;          // current volume
  int volbase;         // original volume (may be temporary changed by tremolo)
  TSample *sample;     // pointer to sample info for the sample we're playing
  size_t position;     // current offset of the sample being outputted to the DAC
  size_t end;          // end offset to detect when we need to repeat
  int pslide;          // amount of periods to slide (up or down, depending upon effect)
  int8_t vslideup;     // amount to increase or decrease for
  int8_t vslidedown;   // volume slide (effect #10)
  int8_t vbspeed;      // vibrato speed
  int8_t vbamp;        // vibrato depth
  uint8_t vbpos;       // position within the vibrato wave sample (0-63)
  int8_t trspeed;      // tremolo speed 
  int8_t tramp;        // tremolo depth
  uint8_t trpos;       // position within the tremolo wave sample (0-63)
  uint16_t noteperiodslideto;  // target period to reach for Portamento effect (03h)
} TChanPlay;

// Information about the current state of the MOD being played
typedef struct
{
  uint8_t format;     // format (PAL or NTSC)
  uint32_t sfreq;     // sampling frequency (defaults to 44100 Hz)
  int finished;       // 1 if MOD has finished playing.
  int newrow;         // 1 if a new division within a pattern has just began
  int newsongpos;     // >=0 if a new position must be loaded into songpos
  int songpos;        // current song position. Goes from 0 to Songlength-1
  int newpatrow;      // >=0 if a new pattern division must be loaded into patrow
  int patrow;         // current division pattern. Goes from 0 to 63
  int tick;           // current tick within a pattern division
  int ticksperdiv;    // how many ticks per division. Defaults to 6.
  int bpm;            // how many BPM. Defaults to 125.
  int vbwave;         // which wave (square, sine, ramp) we're using for vibrato
  int vbretrig;       // 1 if wave position must be resetted on each new division 
  int trwave;         // which wave (square, sine, ramp) we're using for tremolo
  int trretrig;       // 1 if wave position must be resetted on each new division
  size_t tambufplay;  // how many samples to play for this tick
  TChanPlay chan[4];  // playing state info for each channel.
} TModPlay;

// sine, ramp down and square waveforms for both vibrato and tremolo
static int16_t waveforms[3][64] =
{
  {0,24,49,74,97,120,141,161,180,197,212,224,235,244,250,253,255,253,250,244,235,
    224,212,197,180,161,141,120,97,74,49,24,0,-24,-49,-74,-97,-120,-141,-161,-180,
    -197,-212,-224,-235,-244,-250,-253,-255,-253,-250,-244,-235,-224,-212,-197,
    -180,-161,-141,-120,-97,-74,-49,-24},
  {255,246,237,228,219,210,201,192,191,182,173,164,155,146,137,128,127,118,109,
    100,91,82,73,64,63,54,45,36,27,18,9,0,-1,-10,-19,-28,-37,-46,-55,-64,-65,-74,
    -83,-92,-101,-110,-119,-128,-129,-138,-147,-156,-165,-174,-183,-192,-193,
    -202,-211,-220,-229,-238,-247,-255},
  {255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,-255,-255,-255,-255,-255,
    -255,-255,-255,-255,-255,-255,-255,-255,-255,-255,-255,-255,-255,-255,-255,
    -255,-255,-255,-255,-255,-255,-255,-255,-255,-255,-255,-255}
};

// finetune tables (only standard 1-3 octaves)
static uint16_t finetune_table[16][36] =
{
  {
  856,808,762,720,678,640,604,570,538,508,480,453, // C-1 to B-1 Finetune 0
  428,404,381,360,339,320,302,285,269,254,240,226, // C-2 to B-2 Finetune 0
  214,202,190,180,170,160,151,143,135,127,120,113 // C-3 to B-3 Finetune 0
  },
  {
  850,802,757,715,674,637,601,567,535,505,477,450, // C-1 to B-1 Finetune +1
  425,401,379,357,337,318,300,284,268,253,239,225, // C-2 to B-2 Finetune +1
  213,201,189,179,169,159,150,142,134,126,119,113 // C-3 to B-3 Finetune +1
  },
  {
  844,796,752,709,670,632,597,563,532,502,474,447, // C-1 to B-1 Finetune +2
  422,398,376,355,335,316,298,282,266,251,237,224, // C-2 to B-2 Finetune +2
  211,199,188,177,167,158,149,141,133,125,118,112 // C-3 to B-3 Finetune +2
  },
  {
  838,791,746,704,665,628,592,559,528,498,470,444, // C-1 to B-1 Finetune +3
  419,395,373,352,332,314,296,280,264,249,235,222, // C-2 to B-2 Finetune +3
  209,198,187,176,166,157,148,140,132,125,118,111 // C-3 to B-3 Finetune +3
  },
  {
  832,785,741,699,660,623,588,555,524,495,467,441, // C-1 to B-1 Finetune +4
  416,392,370,350,330,312,294,278,262,247,233,220, // C-2 to B-2 Finetune +4
  208,196,185,175,165,156,147,139,131,124,117,110 // C-3 to B-3 Finetune +4
  },
  {
  826,779,736,694,655,619,584,551,520,491,463,437, // C-1 to B-1 Finetune +5
  413,390,368,347,328,309,292,276,260,245,232,219, // C-2 to B-2 Finetune +5
  206,195,184,174,164,155,146,138,130,123,116,109 // C-3 to B-3 Finetune +5
  },
  {
  820,774,730,689,651,614,580,547,516,487,460,434, // C-1 to B-1 Finetune +6
  410,387,365,345,325,307,290,274,258,244,230,217, // C-2 to B-2 Finetune +6
  205,193,183,172,163,154,145,137,129,122,115,109 // C-3 to B-3 Finetune +6
  },
  {
  814,768,725,684,646,610,575,543,513,484,457,431, // C-1 to B-1 Finetune +7
  407,384,363,342,323,305,288,272,256,242,228,216, // C-2 to B-2 Finetune +7
  204,192,181,171,161,152,144,136,128,121,114,108 // C-3 to B-3 Finetune +7
  },
  {
  907,856,808,762,720,678,640,604,570,538,504,480, // C-1 to B-1 Finetune -8
  453,428,404,381,360,339,320,302,285,269,254,240, // C-2 to B-2 Finetune -8
  226,214,202,190,180,170,160,151,143,135,127,120 // C-3 to B-3 Finetune -8
  },
  {
  900,850,802,757,715,675,636,601,567,535,505,477, // C-1 to B-1 Finetune -7
  450,425,401,379,357,337,318,300,284,268,253,238, // C-2 to B-2 Finetune -7
  225,212,200,189,179,169,159,150,142,134,126,119 // C-3 to B-3 Finetune -7
  },
  {
  894,844,796,752,709,670,632,597,563,532,502,474, // C-1 to B-1 Finetune -6
  447,422,398,376,355,335,316,298,282,266,251,237, // C-2 to B-2 Finetune -6
  223,211,199,188,177,167,158,149,141,133,125,118 // C-3 to B-3 Finetune -6
  },
  {
  887,838,791,746,704,665,628,592,559,528,498,470, // C-1 to B-1 Finetune -5
  444,419,395,373,352,332,314,296,280,264,249,235, // C-2 to B-2 Finetune -5
  222,209,198,187,176,166,157,148,140,132,125,118 // C-3 to B-3 Finetune -5
  },
  {
  881,832,785,741,699,660,623,588,555,524,494,467, // C-1 to B-1 Finetune -4
  441,416,392,370,350,330,312,294,278,262,247,233, // C-2 to B-2 Finetune -4
  220,208,196,185,175,165,156,147,139,131,123,117 // C-3 to B-3 Finetune -4
  },
  {
  875,826,779,736,694,655,619,584,551,520,491,463, // C-1 to B-1 Finetune -3
  437,413,390,368,347,338,309,292,276,260,245,232, // C-2 to B-2 Finetune -3
  219,206,195,184,174,164,155,146,138,130,123,116 // C-3 to B-3 Finetune -3
  },
  {
  868,820,774,730,689,651,614,580,547,516,487,460, // C-1 to B-1 Finetune -2
  434,410,387,365,345,325,307,290,274,258,244,230, // C-2 to B-2 Finetune -2
  217,205,193,183,172,163,154,145,137,129,122,115 // C-3 to B-3 Finetune -2
  },
  {
  862,814,768,725,684,646,610,575,543,513,484,457, // C-1 to B-1 Finetune -1
  431,407,384,363,342,323,305,288,272,256,242,228, // C-2 to B-2 Finetune -1
  216,203,192,181,171,161,152,144,136,128,121,114  // C-3 to B-3 Finetune -1
  }
};

static TModule mod;     // global: the complete MOD file as a structure
static TModPlay mplay; // global: the current state of the MOD as we're playing

// Function: finds the name and octave for a note, given its noteperiod and
// stores it into given TChannelData structure (for printing the name of the
// note while playing)
void NotePeriodToNoteName (TChannelData *chd)
{
  int i;
  uint16_t ibest;

  char nombres[12][3] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"}; // note names for standard noteperiods (finetune 0)

  if (chd->Noteperiod == 0)  // if no note here, just spaces, and 0 octave
  {
    strcpy (chd->Note, "  ");
    chd->Octave = 0;
    return;
  }

  ibest = 0;
  for (i=0; i<36; i++)  // find most approximate value for finetune 0 (base) option
  {
    if (chd->Noteperiod == finetune_table[0][i])
      break;
    if (abs(chd->Noteperiod - finetune_table[0][i]) < abs(chd->Noteperiod - finetune_table[0][ibest]))
      ibest = i;
  }

  if (i==36)
    i = ibest;  // if we didn't find an exact match, take the best approximation as result

  chd->Octave = 1 + i/12;
  chd->NoteIndex = i;
  strcpy (chd->Note, nombres[i%12]);
}

// Function: ensures a string is null terminated, and ensures that there is
// no character that isn't valid printable standard ASCII
void Sanitize (char s[], int l)
{
  int i;

  s[l-1] = '\0';  // null terminate the string.
  for (i=0; i<l-1; i++)
  {
    if (s[i]<32 || s[i]>126)  // check whether it's printable standard ASCII
      s[i] = ' ';             // and changes it for a space if not.
  }
}

// Function: loads a MOD file using C standard file functions. Populates
// "mod" global variable. fname is the full pathname of the MOD.
int LoadMOD (char fname[])
{
  FILE *f;
  size_t lfich;
  uint8_t *buffer;
  int numsamples;
  int i, patrow, ch;
  size_t imod;

  f = fopen (fname, "rb");
  if (!f)
    return 0;
  fseek (f, 0, SEEK_END); //
  lfich = ftell(f);       // find out file size
  fseek (f, 0, SEEK_SET); //

  buffer = malloc(lfich);
  fread (buffer, 1, lfich, f); // load the while file into memory
  fclose (f);  // and closes the handle

  // now we use the copy in memory.
  imod = 0;  // index into memory buffer containing the MOD file.
  memcpy (mod.Songname, buffer+imod, 20); // song's name
  Sanitize (mod.Songname, 20);
  imod += 20;

  // check whether this is a 31 instrument MOD, or a 15 instrument MOD.
  if (memcmp (buffer+1080, "M.K.", 4)==0 || memcmp (buffer+1080, "FLT4", 4)==0)
    numsamples = 31;
  else
    numsamples = 15;  // TODO: I should check whether this is a 8 or 16 channel module, and return
                      //  with "unsupported" instead of just assuming it's a 4-channel 15 instrument MOD

  // mod.sample is a 31 element vector, holding all the information about a sample (instrument)
  memset (mod.sample, 0, sizeof mod.sample);  // wipe it
  for (i=0; i<numsamples; i++)
  {
    memcpy (mod.sample[i].Samplename, buffer+imod, 22);  // ASCIIZ name of instrument
    Sanitize (mod.sample[i].Samplename, 22);

    mod.sample[i].Samplelength = 2*(buffer[imod+22]*256+buffer[imod+23]);  // sample length, big endian, word sized, to byte sized, host endian.
    if (mod.sample[i].Samplelength > 0)  // is this an actual sample, or an empty one?
    {
      mod.sample[i].Finetune = buffer[imod+24]; // this is a signed 4 bit number, but I will treat is as an unsigned one (see order of finetune_table)
      mod.sample[i].Volume = buffer[imod+25];  // default volume for sample
      mod.sample[i].Repeatpoint = 2*(buffer[imod+26]*256+buffer[imod+27]);  // repeat point and repeat length are also converted
      mod.sample[i].Repeatlength = 2*(buffer[imod+28]*256+buffer[imod+29]); //  from big endian, word sized, to host endian, byte sized
      mod.sample[i].Sampledata = malloc(mod.sample[i].Samplelength);  // allocate memory for the sample (to be filled later)
    }
    imod += 30; // advance 30 bytes in MOD memory buffer.
  }

  mod.Songlength = buffer[imod]; // how many patterns this song has
  imod += 2;  // skip over the previous data, and a spureous byte nobody knows what it does
  memcpy (mod.Songpositions, buffer+imod, 128);  // copy over the complete 128 byte vector containing the list of patterns to play

  // this section finds the biggest pattern number within the list of pattern (mod.Songpositions vector)
  mod.Numpatterns = mod.Songpositions[0];
  for (i=1; i<128; i++)
    if (mod.Songpositions[i] > mod.Numpatterns)
      mod.Numpatterns = mod.Songpositions[i];
  mod.Numpatterns++;  // mod.Numpatterns stores how many different patterns the song has

  imod += 128;          // skips over the 128 byte vector, and if
  if (numsamples == 31) // a 31 instrument MOD was detected before, skips over
    imod += 4;          // the 31 instrument mark too (characters M.K. or FLT4)

  mod.pattern = malloc (mod.Numpatterns * sizeof *mod.pattern);  // allocate memory for mod.Numpatterns patterns
  for (i=0; i<mod.Numpatterns; i++)  // now populate each one of them
  {
    for (patrow = 0; patrow<64; patrow++)  // a pattern has always 64 rows or divisions
    {
      for (ch=0; ch<4; ch++)  // each row/division has info for 4 channels. Each channel has 4 bytes of info.
      {
        TChannelData *chd = &(mod.pattern[i].row[patrow].chan[ch]);  // pointer to current channel of current row of current pattern, to make coding easier
        chd->Samplenumber = (buffer[imod] & 0xF0) | ((buffer[imod+2]>>4) & 0x0F);  // sample number is scattered over two different bytes
        chd->Noteperiod = (buffer[imod] & 0xF)<<8 | buffer[imod+1];  // noteperiod is a 12 bit unsigned data
        chd->Effect = buffer[imod+2] & 0xF;   // effect number is 4 bits, unsigned
        chd->EffectArg = buffer[imod+3];  // effect argument is 8 bits
        NotePeriodToNoteName (chd);   // complete the info for this channel by translating the noteperiod to a note name and a octave, for printing purposes
        imod += 4;  // we have just processed 4 bytes
      }
    }
  }

  // after patterns, sample data is stored sequentially. Now we can at last,
  // complete mod.sample vector by copying sample data over the allocated memory block
  for (i=0; i<numsamples; i++)  // this iterates over 31 or 15 instruments.
  {
    if (mod.sample[i].Samplelength > 0)  // if there was indeed a sample in this instrument
    {
      memcpy (mod.sample[i].Sampledata, buffer+imod, mod.sample[i].Samplelength);  // copy it
      mod.sample[i].Sampledata[0] = 0;    // first word of sample must be
      mod.sample[i].Sampledata[1] = 0;    // set to zero in player
      imod += mod.sample[i].Samplelength;  // and update mod index position
    }
  }

  free (buffer);  // the MOD file has been processed and stored in a more
                  // organized way into memory. The memory buffer that held the
                  // original MOD file read from disk is no longer needed.
  return 1;
}

// Function: prints on standard output the info for a pattern division, or row,
// in a Protracker style. The format used is this:
// P.RR:  | channel1 data | channel2 data | channel3 data | channel4 data |
// where P is the pattern number, and RR the row number (0 to 63)
// Each channelX data has the following format:
// NNO  II  EAA
// where: NN is the note name ("C ", "C#", "D ", ..., up to "B "). If no note here, then it's "---"
//         O is the octave: normally, 1 to 3.
//        II is the instrument number (1 to 31, decimal). -- if no instrument here
//        E  is the effect number (0 to F). - if no effect here (effect 0 with null argument)
//        AA is the effect argument, two hexadecimal digits (or subeffect + agument, for E effect). -- if no argument and no effect.
void PrintRow (int patnum, int patrow)
{
  int ch;

  printf ("%2d.%2.2d: | ", patnum, patrow);
  for (ch=0; ch<4; ch++)
  {
    TChannelData *chd = &(mod.pattern[patnum].row[patrow].chan[ch]);

    if (chd->Noteperiod != 0)
      printf ("%2.2s%d  ", chd->Note, chd->Octave);
    else
      printf ("---  ");

    if (chd->Samplenumber != 0)
      printf ("%2.2d  ", chd->Samplenumber);
    else
      printf ("--  ");

    if (chd->Effect != 0 || chd->EffectArg != 0)  // if effect number and effect argument are 0, no effect here
      printf ("%1X%2.2X",chd->Effect, chd->EffectArg);
    else
      printf ("---");

    if (ch != 3)
      printf (" | ");
    else
      printf (" |\n");
  }
}

// Function: prints on the standard out the info for a MOD loaded into the
// mod structure.
void InfoMOD (void)
{
  int i;

  printf ("Module name              : %s\n", mod.Songname);
  printf ("Module length            : %d patterns\n", mod.Songlength);
  printf ("Number of unique patterns: %d\n", mod.Numpatterns);
  printf ("Pattern sequence         : ");
  for (i=0; i<mod.Songlength; i++)
    printf ("%2.2d ", mod.Songpositions[i]);
  puts("");

  printf ("Samples:\n");
  for (i=0; i<31; i++)
  {
    if (mod.sample[i].Samplename[0] != 0 || mod.sample[i].Samplelength !=0)
    {
      printf ("%-22.22s  V:%2d  L:%5d   R:%5d %5d  F:%+d\n",
              mod.sample[i].Samplename,
              mod.sample[i].Volume,
              mod.sample[i].Samplelength,
              mod.sample[i].Repeatpoint,
              mod.sample[i].Repeatlength,
              (int)((mod.sample[i].Finetune<8)? mod.sample[i].Finetune : mod.sample[i].Finetune-16));
    }
  }

  puts("");
  /*for (i=0; i<mod.Numpatterns; i++)
  {
    int patrow;
    for (patrow=0; patrow<64; patrow++)
    {
      PrintRow (i, patrow);
    }
    puts("");
  }*/
}


// A series of small functions that implement each one of the effects
// For each effect, a test is made to see if we are at tick 0 (beginning of a division)
// or any other tick, as some effects do some initialization at tick 0, and perform the
// actual effect in the following ticks.

void DoArpeggio_00 (TChannelData *chd, TChanPlay *chan)
{
  // Scaled (fixed point) versions of this sequence: for i=0 to 15: pot[i] = 1 / 2^(i/12)
  // Actually, pot[i] = 2^24 / 2^(i/12). Used to alter the pitch of a note in seminote intervals.
  static uint64_t pot[16] = {16777216,15835583,14946800,14107900,13316085,
                             12568710,11863283,11197448,10568983,9975792,
                             9415894,8887420,8388608,7917791,7473400,7053950};
  uint16_t newperiod;

  if (mplay.tick != 0)
  {
    if (chd->EffectArg != 0)
    {
      switch (mplay.tick % 3)
      {
      case 0:
        newperiod = chan->noteperiod;
        break;
      case 1:
        newperiod = chan->noteperiod * pot[chd->EffectArg & 0xF]/16777216LL;  // new period is calculated from power of two table.
        break;
      case 2:
        newperiod = chan->noteperiod * pot[(chd->EffectArg>>4) & 0xF]/16777216LL;  // new period is calculated from power of two table.
        break;
      default:
        newperiod = chan->noteperiod;
        break;
      }
      chan->fase = ((mplay.format==PAL)? 32768LL * 3546895LL : 32768LL * 3579545LL) / (mplay.sfreq * newperiod);  // and used to calculate new phase for phase-accum counter
    }
  }
}

void DoSlideUp_01 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
    chan->pslide = chd->EffectArg;
  else
  {
    if (chan->noteperiod - chan->pslide >= finetune_table[chan->finetune][35])  // if current noteperiod minus amount of sliding does not go beyond B-3 ...
      chan->noteperiod -= chan->pslide;  // apply portamento
    else
      chan->noteperiod  = finetune_table[chan->finetune][35];  // else stays at B-3
    chan->fase = ((mplay.format==PAL)? 32768LL * 3546895LL : 32768LL * 3579545LL) / (mplay.sfreq * chan->noteperiod);  // calculate new phase
  }
}

void DoSlideDown_02 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
    chan->pslide = chd->EffectArg;
  else
  {
    if (chan->noteperiod + chan->pslide <= finetune_table[chan->finetune][0])  // if current noteperiod minus amount of sliding does not go beyond C-1 ...
      chan->noteperiod += chan->pslide;  // apply portamento
    else
      chan->noteperiod  = finetune_table[chan->finetune][0];  // else stays at C-1
    chan->fase = ((mplay.format==PAL)? 32768LL * 3546895LL : 32768LL * 3579545LL) / (mplay.sfreq * chan->noteperiod);  // calculate new phase
  }                                                    // remember that the phase-accum counter has a 15 bit accum, so phase must be shifted 15 bits left,                       
}                                                      // or multiplied by 32768

void DoSlideToNote_03 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    if (chd->Noteperiod != 0)
      chan->noteperiodslideto = finetune_table[chan->finetune][chd->NoteIndex];  // new target for Portamento
    if (chd->EffectArg != 0)   // if the sliding speed is also present, store it as well.
      chan->pslide = chd->EffectArg;
  }
  else
  {
    if (chan->noteperiod < chan->noteperiodslideto)                    // relation between current period and target period determines portamento direction
    {
      if (chan->noteperiod + chan->pslide <= chan->noteperiodslideto)  // if we haven't reached the target note period
        chan->noteperiod += chan->pslide;                              // keep aplying portamento up
      else
        chan->noteperiod = chan->noteperiodslideto;                    // else stays at the target note period
    }
    else if (chan->noteperiod > chan->noteperiodslideto)               // same for portamento down
    {
      if (chan->noteperiod - chan->pslide >= chan->noteperiodslideto)
        chan->noteperiod -= chan->pslide;
      else
        chan->noteperiod = chan->noteperiodslideto;
    }
    chan->fase = ((mplay.format==PAL)? 32768LL * 3546895LL : 32768LL * 3579545LL) / (mplay.sfreq * chan->noteperiod);
  }
}

void DoVibrato_04 (TChannelData *chd, TChanPlay *chan)
{
  // Every oscillator waveform is 64 points long, and the speed parameter
  // denotes by how many points per tick the play position is advanced.
  // So at a vibrato speed of 2, the vibrato waveform repeats after 32 ticks.
  // The Random waveforms are not supported by ProTracker and FastTracker.
  // While they are supported by some MOD / XM players, they should be avoided.
  if (mplay.tick == 0)
  {
    if (((chd->EffectArg>>4) & 0xF) != 0)
      chan->vbspeed = (chd->EffectArg>>4) & 0xF;
    if ((chd->EffectArg & 0xF) != 0)
      chan->vbamp = chd->EffectArg & 0xF;
    if (mplay.vbretrig == 1)
      chan->vbpos = 0;
  }
  else
  {
    uint16_t newperiod = chan->noteperiod + waveforms[mplay.vbwave][chan->vbpos] * chan->vbamp / 128L;
    chan->vbpos = (chan->vbpos + chan->vbspeed) & 0x3F;
    chan->fase = ((mplay.format==PAL)? 32768LL * 3546895LL : 32768LL * 3579545LL) / (mplay.sfreq * newperiod);  // and used to calculate new phase for phase-accum counter
  }
}

// Tremolo is calculated much the same way as vibrato is.
void DoTremolo_07 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    if (((chd->EffectArg>>4) & 0xF) != 0)
      chan->trspeed = (chd->EffectArg>>4) & 0xF;
    if ((chd->EffectArg & 0xF) != 0)
      chan->tramp = chd->EffectArg & 0xF;
    if (mplay.trretrig == 1)
      chan->trpos = 0;
  }
  else
  {
    int16_t newvol = chan->volbase + waveforms[mplay.trwave][chan->trpos] * chan->tramp / 64L;
    newvol = (newvol<0)? 0 : (newvol>64)? 64 : newvol;
    chan->trpos = (chan->trpos + chan->trspeed) & 0x3F;
    chan->volume = newvol;
  }
}

void DoVolumeSlide_10 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    chan->vslideup = (chd->EffectArg & 0xF0)>>4; // volume slide up, or down
    chan->vslidedown = (chd->EffectArg & 0xF);   // (only one of them must be non zero)
  }
  else
  {
    if (chan->vslideup != 0 && chan->volume + chan->vslideup <= 64)
      chan->volume += chan->vslideup;
    else if (chan->vslidedown != 0 && chan->volume - chan->vslidedown >= 0)
      chan->volume -= chan->vslidedown;
    chan->volbase = chan->volume;
  }
}

void DoSlideToNoteAndVolumeSlide_05 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)  // slide to tone effect here must not store the sliding value, as the effect argument here is volume sliding
  {
    if (chd->Noteperiod != 0)
      chan->noteperiodslideto = finetune_table[chan->finetune][chd->NoteIndex];  // new target for Portamento
  }
  else
    DoSlideToNote_03 (chd, chan);

  DoVolumeSlide_10 (chd, chan);
}

void DoVibratoAndVolumeSlide_06 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    if (mplay.vbretrig == 1)
      chan->vbpos = 0;
  }
  else
    DoVibrato_04 (chd, chan);
  DoVolumeSlide_10 (chd, chan);
}

void DoSampleOffset_09 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    if (chd->EffectArg != 0)         // sample offset. argument is high byte of new offset.
      chan->faseacum = (chd->EffectArg * 256)<<15; // Store it into the phase-accumulator counter
  }
}

void DoJumpSongposition_11 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    mplay.newsongpos = chd->EffectArg;  // jump to new song position.
    mplay.newpatrow = 0;                // we start from division 0
  }
}

void DoVolume_12 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    chan->volume = chd->EffectArg;      // new volume for this channel
    chan->volbase = chan->volume;
  }
}

void DoPatternBreak_13 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    mplay.newsongpos = mplay.songpos + 1;  // pattern break. We jump to the next song position
    mplay.newpatrow = ((chd->EffectArg >> 4)&0x0F)*10+(chd->EffectArg & 0xF);  // and a certain division, given in BCD!
  }
}

void DoFineSlideUp_14_01 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    mplay.tick = 1;  // dirty trick to fool DoSlideUp_01() so it does the actual sliding
    chan->pslide = chd->EffectArg & 0xF;
    DoSlideUp_01 (chd, chan);
    mplay.tick = 0;  // back to its true value
  }
}

void DoFineSlideDown_14_02 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    mplay.tick = 1;  // dirty trick to fool DoSlideDown_02() so it does the actual sliding
    chan->pslide = chd->EffectArg & 0xF;
    DoSlideDown_02 (chd, chan);
    mplay.tick = 0;  // back to its true value
  }
}

void DoSetVibratoWaveform_14_04 (TChannelData *chd, TChanPlay *chan)
{
  mplay.vbwave = chd->EffectArg & 0x3;
  if (mplay.vbwave == 3)
    mplay.vbwave = rand()%3;
  mplay.vbretrig = (chd->EffectArg & 0x4)? 0 : 1;
}

void DoSetFinetune_14_05 (TChannelData *chd, TChanPlay *chan)
{
  chan->sample->Finetune = chd->EffectArg & 0xF;
}

void DoSetTremoloWaveform_14_07 (TChannelData *chd, TChanPlay *chan)
{
  mplay.trwave = chd->EffectArg & 0x3;
  if (mplay.trwave == 3)
    mplay.trwave = rand()%3;
  mplay.trretrig = (chd->EffectArg & 0x4)? 0 : 1;
}

void DoNoteRetrig_14_09 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == (chd->EffectArg & 0x0F))
  {
    chan->faseacum = 0;
    chan->position = 0;
  }
}

void DoFineVolumeSlideUp_14_10 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    if (chan->volume + (chd->EffectArg & 0x0F) <= 64)
      chan->volume += (chd->EffectArg & 0x0F);
    else
      chan->volume = 64;
    chan->volbase = chan->volume;
  }
}

void DoFineVolumeSlideDown_14_11 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    if (chan->volume - (chd->EffectArg & 0x0F) >= 0)
      chan->volume -= (chd->EffectArg & 0x0F);
    else
      chan->volume = 0;
    chan->volbase = chan->volume;
  }
}

void DoCutNote_14_12 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick > (chd->EffectArg & 0xF))
    chan->volume = 0;
}

void DoDelayNote_14_13 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 1+(chd->EffectArg & 0xF))
  {
    chan->volume = chan->volbase;
    chan->faseacum = 0;                         // init counters
    chan->position = 0;
    chan->fase = ((mplay.format==PAL)? 32768LL * 3546895LL : 32768LL * 3579545LL) / (mplay.sfreq * chan->noteperiod);  // calculate phase for counter
  }
  else
  {
    chan->volume = 0;
    chan->faseacum = 0;                         // init counters
    chan->position = 0;
    chan->fase = 0;
  }
}

void DoSetSpeedBPM_15 (TChannelData *chd, TChanPlay *chan)
{
  if (mplay.tick == 0)
  {
    if (chd->EffectArg<32)  // if it's under 32, the it's number of ticks per division.
    {
      mplay.ticksperdiv = chd->EffectArg;
    }
    else  // else, it's the number of bpm. A beat is 4 divisions
    {
      mplay.bpm = chd->EffectArg;
      mplay.tambufplay = (mplay.sfreq*15L)/(6*mplay.bpm);
      // for some reason (???), 6 ticks per division must be used for this
      // calculation, although the actual ticks per division rate may be
      // different
    }
  }
}

// Function: process the effects for the current tick, in a given channel within a given division
// within a given pattern (chd) and the information of that channel while being played (chan)
void ProcessEffect (TChannelData *chd, TChanPlay *chan)
{
  uint8_t SubEffect = (chd->EffectArg >> 4) & 0xF;

  switch (chd->Effect)
  {
  case 0:  DoArpeggio_00                  (chd, chan); break;
  case 1:  DoSlideUp_01                   (chd, chan); break;
  case 2:  DoSlideDown_02                 (chd, chan); break;
  case 3:  DoSlideToNote_03               (chd, chan); break;
  case 4:  DoVibrato_04                   (chd, chan); break;
  case 5:  DoSlideToNoteAndVolumeSlide_05 (chd, chan); break;
  case 6:  DoVibratoAndVolumeSlide_06     (chd, chan); break;
  case 7:  DoTremolo_07                   (chd, chan); break;
  case 9:  DoSampleOffset_09              (chd, chan); break;
  case 10: DoVolumeSlide_10               (chd, chan); break;
  case 11: DoJumpSongposition_11          (chd, chan); break;
  case 12: DoVolume_12                    (chd, chan); break;
  case 13: DoPatternBreak_13              (chd, chan); break;
  case 14:  // miscellaneous effects.
    switch (SubEffect)
    {
    case 1:  DoFineSlideUp_14_01          (chd, chan); break;
    case 2:  DoFineSlideDown_14_02        (chd, chan); break;
    case 4:  DoSetVibratoWaveform_14_04   (chd, chan); break;
    case 5:  DoSetFinetune_14_05          (chd, chan); break;
    case 7:  DoSetTremoloWaveform_14_07   (chd, chan); break;
    case 9:  DoNoteRetrig_14_09           (chd, chan); break;
    case 10: DoFineVolumeSlideUp_14_10    (chd, chan); break;
    case 11: DoFineVolumeSlideDown_14_11  (chd, chan); break;
    case 12: DoCutNote_14_12              (chd, chan); break;
    case 13: DoDelayNote_14_13            (chd, chan); break;
    }
    break;
  case 15: DoSetSpeedBPM_15               (chd, chan); break;
  }
}

// Function: does all the needed job to get a block of samples ready to be
// played by the sound card in one tick.
void PlayTick (void)
{
  static uint8_t sbuffer[44100];  // up to about 1 second of audio
  int i, ch;
  int muestra, mezcla;
  uint8_t muestrafinal;

  if (mplay.finished)  // if MOD has finished, do nothing.
    return;

  if (mplay.tick >= mplay.ticksperdiv)  // if we have finished a division...
  {
    mplay.tick = 0;   // beginning of a new division
    if (mplay.newpatrow >= 0 || mplay.newsongpos >= 0)  // need to jump to another division or song position?
    {
      if (mplay.newpatrow >= 0)  // do so for the new division
      {
        mplay.patrow = mplay.newpatrow;
        mplay.newpatrow = -1;
      }
      if (mplay.newsongpos >= 0)  // and the new song position
      {
        mplay.songpos = mplay.newsongpos;
        mplay.newsongpos = -1;
      }
    }
    else if (mplay.patrow >= 63)  // ran out of divisions in the current pattern?
    {
      mplay.patrow = 0;  // go to the beginning of...
      mplay.songpos++;   // a new pattern
    }
    else
      mplay.patrow++;  // else, just go to the next division in the current pattern

    if (mplay.songpos >= mod.Songlength)  // ran out of patterns in the song?
    {
      mplay.finished = 1;  // then, signal it as finished
      return;
    }
  }

  if (mplay.tick == 0)
    mplay.newrow = 1;  // signal the user program that a new division has started

  for (ch=0; ch<4; ch++)  // now process each channel
  {
    TChannelData *chd = &(mod.pattern[mod.Songpositions[mplay.songpos]].row[mplay.patrow].chan[ch]);
    if (mplay.tick == 0)  // first tick in the division?
    {
      if (chd->Samplenumber != 0)  // retrieve sample data for current instrument, if given.
      {
        mplay.chan[ch].sample = &(mod.sample[chd->Samplenumber-1]);
        mplay.chan[ch].finetune = mod.sample[chd->Samplenumber-1].Finetune;
        mplay.chan[ch].end = mod.sample[chd->Samplenumber-1].Samplelength;
        mplay.chan[ch].volume = mod.sample[chd->Samplenumber-1].Volume;
        mplay.chan[ch].volbase = mplay.chan[ch].volume;
      }
      if (chd->Noteperiod != 0 && chd->Effect != 3 && chd->Effect != 5)  // calculate values for phase-accumulator counter from the current noteperiod.
      {                                              // except if effect number is 3 or 5 (Portamento to note), because notepriod is then an argument to that effect
        uint16_t ActualNotePeriod = finetune_table[mplay.chan[ch].finetune][chd->NoteIndex];
        mplay.chan[ch].noteperiodslideto = ActualNotePeriod;  // this may be a new target for Portamento to note after all
        mplay.chan[ch].noteperiod = ActualNotePeriod;
        mplay.chan[ch].faseacum = 0;                         // init counters
        mplay.chan[ch].position = 0;
        mplay.chan[ch].fase = ((mplay.format==PAL)? 32768LL * 3546895LL : 32768LL * 3579545LL) / (mplay.sfreq * ActualNotePeriod);  // calculate phase for counter
      }
    }
    ProcessEffect (chd, &mplay.chan[ch]);  // after processing the channel for tick 0, process any effect in the channel (all ticks)
  }

  // all data for current tick has been updated. Now, using current instruments and current phase-accum values, retrieve and
  // mix all the samples needed to fill the sound buffer for this tick.
  for (i=0; i<mplay.tambufplay; i++)  // repeat until the buffer is full
  {
    mezcla = 0;  // mix of all channels
    for (ch=0; ch<4; ch++)  // proceed with each of them
    {
      if (mplay.chan[ch].sample == NULL || mplay.chan[ch].sample->Sampledata == NULL)  // if instrument is silence, just don't add anything to the mix
        continue;
      muestra = mplay.chan[ch].sample->Sampledata[mplay.chan[ch].position] * mplay.chan[ch].volume;  // this is the current sample from the instrument, after being scaled according to the current channel volume
      mplay.chan[ch].faseacum += mplay.chan[ch].fase;           // now update offset to sample data for this instrument
      mplay.chan[ch].position = mplay.chan[ch].faseacum >> 15;  // by using the result from the phase-accumulator counter
      if (mplay.chan[ch].position >= mplay.chan[ch].end)        // check if we need to loop the instrument
      {
        mplay.chan[ch].faseacum = (mplay.chan[ch].sample->Repeatpoint << 15);    // go to the first repeat position
        mplay.chan[ch].position = mplay.chan[ch].sample->Repeatpoint;
        mplay.chan[ch].end = mplay.chan[ch].sample->Repeatpoint + mplay.chan[ch].sample->Repeatlength;  // and mark the new instrument end as the end of repetition
      }
      mezcla += muestra;  // add the sample to the mix
    }
    muestrafinal = 128 + (mezcla / (4*64));  // average the final mix, and convert to an unsigned 8-bit value for sound card
    sbuffer[i] = muestrafinal;
  }
  ReproducirAudio (sbuffer, mplay.tambufplay);  // send the block to the audio device

  mplay.tick++;
}

int BeginPlayMOD (uint32_t sfreq)
{
  int ch, i;

  memset (mplay.chan, 0, sizeof mplay.chan);  // init the mod.chan table
  for (ch=0; ch<4; ch++)
  {
    mplay.chan[ch].volume = 64;  // defaults to max volume for each channel (maybe not needed after all)
  }
  // init MOD play defaults
  mplay.sfreq = sfreq;
  mplay.songpos = 0;
  mplay.patrow = 0;
  mplay.newsongpos = -1;
  mplay.newpatrow = -1;
  mplay.ticksperdiv = 6;
  mplay.bpm = 125;
  mplay.vbwave = 0;
  mplay.vbretrig = 1;
  mplay.trwave = 0;
  mplay.trretrig = 1;
  mplay.tambufplay = (sfreq*15L)/(mplay.ticksperdiv*mplay.bpm);  // 125 bpm, sfreq Hz, 6 ticks/div
  mplay.finished = 0;

  // open audio device with a user callback function which will be executed
  // each time an audio block has finished playing
  if (AbrirAudioCallBack (mplay.sfreq, &PlayTick) != 0)
    return 0;

  // now all audio buffers are empty, so we fill all of them
  for (i=0; i<MAXAUDIOBUFFERS; i++)
    PlayTick();
  return 1;
}

// Function: finishes MOD audio playing.
void EndPlayMOD (void)
{
  mplay.finished = 1;
  CerrarAudio();
}

// main function. Retrieves MOD file name and optional sampling frequency
// from user arguments, then load the MOD, display some info about it, and then,
// it starts playing it (in background). Meanwhile, the main function continues
// in a loop printing new pattern divisions as they are being played, while
// waiting for the song to finish or the user to press the ESC key.
int main (int argc, char *argv[])
{
  int res, i, tecla;
  char fname[256] = "";
  uint32_t sfreq = 32000;  // minimum sampling frequency to play MODs without aliasing.

  mplay.format = PAL;

  for (i=1; i<argc; i++)
  {
    if (strlen(argv[i])>2 && argv[i][0]=='-')
    {
      switch (argv[i][1])
      {
      case 'f':
        sfreq = atoi(argv[i]+2);
        break;
      }
    }
    else
      strcpy (fname, argv[i]);
  }
  if (fname[0] == 0)
  {
    printf ("Need MOD file name. Aborting.\n");
    return 0;
  }

  if (strlen(fname)<4 || stricmp (fname + strlen(fname) - 4, ".MOD")!=0)
    strcat (fname, ".MOD");

  res = LoadMOD (fname);
  if (res != 1)
  {
    printf ("[%s] module not found, or error during loading.\n", fname);
    return 0;
  }

  InfoMOD ();
  if (BeginPlayMOD (sfreq) != 1)
  {
    printf ("ERROR opening audio device.\n");
    return 0;
  }

  // Now the MOD has begun playing in the background.
  // We can monitor it by peeking values from mplay variable.
  // Variable mplay.newrow is set whenever a new division has begun playing
  // but this variable doesn't get updated for the very first division, so
  // we print it ahead.
  // Once the row has been printed, we must reset mplay.newrow and wait for it
  // to be setted again.
  PrintRow (mod.Songpositions[0], 0);
  mplay.newrow = 0;
  while (mplay.finished == 0)
  {
    if (mplay.newrow == 1)
    {
      PrintRow (mod.Songpositions[mplay.songpos], mplay.patrow);
      mplay.newrow = 0;
    }
    if (_kbhit())
    {
      tecla = _getch();
      if (tecla == 27)
        break;
      if (tecla == 'a' && mplay.songpos != mod.Songlength-1)
      {
        mplay.newsongpos = mplay.songpos + 1;
        mplay.newpatrow = 0;
      }
    }
  }

  EndPlayMOD();
  return 0;
}
