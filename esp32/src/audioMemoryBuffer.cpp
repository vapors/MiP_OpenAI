#include "audioMemoryBuffer.h"
#include <Arduino.h>

AudioMemoryBuffer::AudioMemoryBuffer() {
    buffer = (int16_t*)malloc(BUFFER_SIZE * sizeof(int16_t));
    if (!buffer) {
        Serial.println("Failed to allocate audio buffer memory");
    }
    clear();
}

bool AudioMemoryBuffer::write(const int16_t* data, int length) {
    if (!buffer || !data) {
        return false;
    }
    
    if (samplesAvailable + length > BUFFER_SIZE) {
        return false; // Buffer would overflow
    }
    
    for (int i = 0; i < length; i++) {
        buffer[writeIndex] = data[i];
        writeIndex = (writeIndex + 1) % BUFFER_SIZE;
    }
    samplesAvailable += length;
    return true;
}

bool AudioMemoryBuffer::read(int16_t* data, int length) {
    if (!buffer || !data) {
        return false;
    }

    if (samplesAvailable < length) {
        return false; // Not enough data available
    }
    
    for (int i = 0; i < length; i++) {
        data[i] = buffer[readIndex];
        readIndex = (readIndex + 1) % BUFFER_SIZE;
    }
    samplesAvailable -= length;
    return true;
}

int AudioMemoryBuffer::available() const {
    if (!buffer) {
        return 0;
    }
    return samplesAvailable;
}

void AudioMemoryBuffer::clear() {
    if (!buffer) {
        return;
    }
    writeIndex = 0;
    readIndex = 0;
    samplesAvailable = 0;
    memset(buffer, 0, BUFFER_SIZE * sizeof(int16_t));
}