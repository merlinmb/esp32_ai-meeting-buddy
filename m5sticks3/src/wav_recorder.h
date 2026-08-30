#pragma once
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <vector>
#include "pins.h"
#include "config.h"

// Handles: SD init, opening a new WAV file, writing PCM buffers, and
// finalizing the WAV header (file size isn't known until recording stops,
// so the 44-byte header is written twice - once as a placeholder, once with
// real sizes at close()).

#pragma pack(push, 1)
struct WavHeader {
  char     riff[4]      = {'R','I','F','F'};
  uint32_t chunkSize     = 0;              // filled in at close()
  char     wave[4]      = {'W','A','V','E'};
  char     fmt[4]       = {'f','m','t',' '};
  uint32_t fmtSize       = 16;
  uint16_t audioFormat   = 1;              // PCM
  uint16_t numChannels   = AUDIO_CHANNELS;
  uint32_t sampleRate    = SAMPLE_RATE_HZ;
  uint32_t byteRate      = SAMPLE_RATE_HZ * AUDIO_CHANNELS * (SAMPLE_BITS / 8);
  uint16_t blockAlign    = AUDIO_CHANNELS * (SAMPLE_BITS / 8);
  uint16_t bitsPerSample = SAMPLE_BITS;
  char     data[4]       = {'d','a','t','a'};
  uint32_t dataSize      = 0;              // filled in at close()
};
#pragma pack(pop)

class WavRecorder {
 public:
  bool beginSd() {
    SPI.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) {
      Serial.println("SD.begin failed (CS=" + String(PIN_SD_CS) +
                      ", SCLK=" + String(PIN_SD_SCLK) +
                      ", MOSI=" + String(PIN_SD_MOSI) +
                      ", MISO=" + String(PIN_SD_MISO) + ")");
      return false;
    }
    return true;
  }

  // Starts a new recording; filenameBase should NOT include ".wav".
  bool startNewFile(const String &filenameBase) {
    _path = "/" + filenameBase + ".wav";
    _file = SD.open(_path, FILE_WRITE);
    if (!_file) return false;

    WavHeader hdr;  // placeholder, corrected in close()
    _file.write((const uint8_t *)&hdr, sizeof(hdr));
    _bytesWritten = 0;
    return true;
  }

  bool isOpen() { return (bool)_file; }

  size_t write(const uint8_t *buf, size_t len) {
    if (!_file) return 0;
    size_t n = _file.write(buf, len);
    _bytesWritten += n;
    return n;
  }

  // Rewrites the header with real sizes and closes the file.
  void close() {
    if (!_file) return;
    WavHeader hdr;
    hdr.dataSize = _bytesWritten;
    hdr.chunkSize = 36 + _bytesWritten;
    _file.seek(0);
    _file.write((const uint8_t *)&hdr, sizeof(hdr));
    _file.close();
  }

  String lastPath() const { return _path; }
  uint32_t bytesWritten() const { return _bytesWritten; }

  // Deletes the last file written by close() - used to discard recordings
  // too short to be useful.
  void discardLast() {
    if (_path.length()) SD.remove(_path);
  }

  // Lists .wav files in the root directory - files already uploaded live
  // under /uploaded (see WifiUploader::moveToUploaded) so they never appear here.
  static std::vector<String> pendingFiles() {
    std::vector<String> out;
    File root = SD.open("/");
    if (!root) return out;
    File f = root.openNextFile();
    while (f) {
      String name = f.name();
      if (name.endsWith(".wav")) {
        out.push_back(name.startsWith("/") ? name : "/" + name);
      }
      f = root.openNextFile();
    }
    return out;
  }

  // Deletes every .wav recording in the root directory. Returns the count of
  // .wav files removed.
  static int deleteAllRecordings() {
    int count = 0;
    File root = SD.open("/");
    if (!root) return count;
    File f = root.openNextFile();
    while (f) {
      String name = f.name();
      bool isDir = f.isDirectory();
      f.close();
      if (!isDir && name.endsWith(".wav")) {
        String path = name.startsWith("/") ? name : "/" + name;
        SD.remove(path);
        count++;
      }
      f = root.openNextFile();
    }
    return count;
  }

 private:
  File _file;
  String _path;
  uint32_t _bytesWritten = 0;
};
