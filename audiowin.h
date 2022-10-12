#ifndef __AUDIOWIN_H__
#define __AUDIOWIN_H__

#include <stdint.h>
#include <windows.h>
#include <mmsystem.h>

#define SFREQ 44100

#ifndef MAXAUDIOBUFFERS
#define MAXAUDIOBUFFERS 4
#endif

typedef void (*TFuncionCBUsuario)(void);

static HWAVEOUT wout;
static WAVEFORMATEX wfx;
static HANDLE evento_fin_play = 0;
static WAVEHDR wh[MAXAUDIOBUFFERS];
static TFuncionCBUsuario pfucb = NULL;

static void CALLBACK FuncionCallbackAudio (HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
  LPWAVEHDR lpwh;
  
  switch (uMsg)
  {
  case WOM_OPEN:
    break;
    
  case WOM_DONE:   
    lpwh = (WAVEHDR *)dwParam1;
    lpwh->dwFlags &= ~WHDR_PREPARED;
    lpwh->dwUser = 1;
    SetEvent (evento_fin_play);
    if (pfucb)
      pfucb();
    break;
  }
}

int AbrirAudioCallBack (uint32_t sfreq, TFuncionCBUsuario p)
{
  MMRESULT res;
  int i;
  
  pfucb = p;
  
  evento_fin_play = CreateEvent(0, FALSE, FALSE, 0);
  if (!evento_fin_play)
    return 0;
  
  for (i = 0; i<MAXAUDIOBUFFERS; i++)
  {
    memset (&wh[i], 0, sizeof wh[i]);	
    wh[i].dwUser = 1;
  }
  
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = 1;
  wfx.nSamplesPerSec = sfreq;
  wfx.wBitsPerSample = 8;
  wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
  wfx.cbSize = 0;
  
  res = waveOutOpen (&wout, WAVE_MAPPER, &wfx, (DWORD_PTR)FuncionCallbackAudio, 0, CALLBACK_FUNCTION);
  return res;
}

int AbrirAudio (void)
{
  return AbrirAudioCallBack (44100, NULL);
}

void CerrarAudio (void)
{
  int i;
  
  pfucb = NULL;
  for (i=0; i<MAXAUDIOBUFFERS; i++)
    if (wh[i].dwUser == 0)
      WaitForSingleObject (evento_fin_play, INFINITE);
  
  waveOutClose (wout);
  CloseHandle (evento_fin_play);
  for (i=0; i<MAXAUDIOBUFFERS; i++)
  {
    if (wh[i].lpData)
    {
      free (wh[i].lpData);
      wh[i].lpData = NULL;
    }
  }
  wout = 0;
}

void ReproducirAudio (uint8_t *data, int ldata)
{
  int i;
    
  while (1)
  {
    for (i=0; i<MAXAUDIOBUFFERS; i++)
    {
      if (wh[i].dwUser == 1)
      {
        waveOutUnprepareHeader (wout, &wh[i], sizeof wh[i]);
        if (wh[i].lpData)
          free (wh[i].lpData);
        wh[i].lpData = NULL;
        break;
      }
    }
    if (i == MAXAUDIOBUFFERS)
    {
      WaitForSingleObject (evento_fin_play, INFINITE);
    }
    else
      break;
  }

  memset (&wh[i], 0, sizeof wh[i]);
  wh[i].lpData = malloc (ldata * sizeof *wh[i].lpData);
  memcpy (wh[i].lpData, data, ldata);
  wh[i].dwBufferLength = ldata;
  wh[i].dwLoops = 1;

  waveOutPrepareHeader (wout, &wh[i], sizeof wh[i]);	
  waveOutWrite (wout, &wh[i], sizeof wh[i]);  
}

#endif
