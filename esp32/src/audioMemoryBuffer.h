#ifndef AUDIO_MEMORY_BUFFER_H
#define AUDIO_MEMORY_BUFFER_H

#include <cstdint>

class AudioMemoryBuffer {
private:
    static const int BUFFER_SIZE = 32768; // 32Ko buffer size
    int16_t* buffer;
    int writeIndex = 0;
    int readIndex = 0; 
    int samplesAvailable = 0;

public:
    AudioMemoryBuffer();
    bool write(const int16_t* data, int length);
    bool read(int16_t* data, int length);
    int available() const;
    void clear();
};

#endif // AUDIO_MEMORY_BUFFER_H
