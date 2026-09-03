#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "record.h"
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "notes.h"
#include "../../sounds.h"

extern "C" {
#include "../../src/audio/audio_bsp.h"
}

bool record() {
  int num = nextNoteNumber();
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, num);
  Serial.printf("[Rec] %s\n", path);

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return false;

  uint8_t header[44]={}; f.write(header, 44);

  int16_t* sbuf = (int16_t*)heap_caps_malloc(REC_BUF,   MALLOC_CAP_8BIT);
  int16_t* mbuf = (int16_t*)heap_caps_malloc(REC_BUF/2, MALLOC_CAP_8BIT);
  if (!sbuf||!mbuf) {
    if(sbuf)heap_caps_free(sbuf); if(mbuf)heap_caps_free(mbuf);
    f.close(); return false;
  }

  uint32_t totalMono=0, t0=millis();

  while (digitalRead(BTN_REC)==LOW || millis()-t0 < 500) {
    audio_playback_read((void*)sbuf, REC_BUF);
    int mono = REC_BUF/4;
    for (int i=0;i<mono;i++) mbuf[i] = sbuf[i*2];
    size_t written = f.write((uint8_t*)mbuf, mono*2);
    if (written == 0) break;
    totalMono += written;
  }

  heap_caps_free(sbuf); heap_caps_free(mbuf);

  f.seek(0);
  uint32_t dB=totalMono, fS=dB+36, bR=SAMPLE_RATE*2;
  uint16_t bA=2,aF=1,ch=1,bps=16; uint32_t fL=16,sr=SAMPLE_RATE;
  f.write((uint8_t*)"RIFF",4); f.write((uint8_t*)&fS,4);
  f.write((uint8_t*)"WAVE",4); f.write((uint8_t*)"fmt ",4);
  f.write((uint8_t*)&fL,4);   f.write((uint8_t*)&aF,2);
  f.write((uint8_t*)&ch,2);   f.write((uint8_t*)&sr,4);
  f.write((uint8_t*)&bR,4);   f.write((uint8_t*)&bA,2);
  f.write((uint8_t*)&bps,2);
  f.write((uint8_t*)"data",4); f.write((uint8_t*)&dB,4);
  f.close();

  lastRecNum = num;
  Serial.printf("[Rec] done: %lu bytes\n", (unsigned long)totalMono);
  return totalMono > 1000;
}

bool playWavFile(const char* path) {
  File f = SD_MMC.open(path);
  if (!f) return false;
  if (f.size() <= 44) { f.close(); return false; }

  f.seek(44);

  const int monoBytes = 1024;
  uint8_t* monoBuf   = (uint8_t*)heap_caps_malloc(monoBytes,     MALLOC_CAP_8BIT);
  int16_t* stereoBuf = (int16_t*)heap_caps_malloc(monoBytes * 2, MALLOC_CAP_8BIT);

  if (!monoBuf || !stereoBuf) {
    if (monoBuf)   heap_caps_free(monoBuf);
    if (stereoBuf) heap_caps_free(stereoBuf);
    f.close();
    return false;
  }

  audioPlaying  = true;
  stopPlayback  = false;

  palaSoundSetEnabled(false);
  audio_playback_set_vol(85);

  while (f.available() && !stopPlayback) {
    int readBytes = f.read(monoBuf, monoBytes);
    if (readBytes <= 0) break;
    if (readBytes & 1) readBytes--;

    int samples = readBytes / 2;
    int16_t* mono = (int16_t*)monoBuf;
    for (int i = 0; i < samples; i++) {
      int16_t s = mono[i];
      stereoBuf[i * 2 + 0] = s;
      stereoBuf[i * 2 + 1] = s;
    }
    audio_playback_write((void*)stereoBuf, (uint32_t)(samples * 2 * sizeof(int16_t)));

    if (digitalRead(BTN_REC) == LOW) {
      delay(20);
      if (digitalRead(BTN_REC) == LOW) {
        while (digitalRead(BTN_REC) == LOW) delay(5);
        stopPlayback = true;
      }
    }
  }

  audio_playback_set_vol(0);
  palaSoundSetEnabled(true);

  heap_caps_free(monoBuf);
  heap_caps_free(stereoBuf);
  f.close();

  audioPlaying = false;
  stopPlayback = false;
  return true;
}
