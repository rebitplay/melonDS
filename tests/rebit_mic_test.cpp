#include "../src/libretro/rebit_mic.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

int main()
{
    RebitMicBuffer mic;
    int16_t frame[RebitMicBuffer::FrameSamples];
    int16_t samples[] = {-32768, -10, 0, 10, 32767};
    mic.push(samples, 5);
    assert(mic.size() == 0);
    mic.set_active(true);
    mic.push(nullptr, 5);
    mic.push(samples, -1);
    mic.push(samples, 5);
    assert(mic.read_frame(frame) == 5);
    for (int i = 0; i < 5; i++) assert(frame[i] == samples[i]);
    for (int i = 5; i < RebitMicBuffer::FrameSamples; i++) assert(frame[i] == 0);
    assert(mic.read_frame(frame) == 0);
    for (auto value : frame) assert(value == 0);

    std::vector<int16_t> large(RebitMicBuffer::Capacity + 1000);
    for (unsigned i = 0; i < large.size(); i++) large[i] = i;
    mic.push(large.data(), large.size());
    assert(mic.size() == RebitMicBuffer::Capacity);
    assert(mic.read_frame(frame) == RebitMicBuffer::FrameSamples);
    for (int i = 0; i < RebitMicBuffer::FrameSamples; i++) assert(frame[i] == i + 1000);
    mic.push(samples, 5);
    mic.set_active(false);
    mic.set_active(true);
    assert(mic.read_frame(frame) == 0);

    // Exercise wraparound and overflow across many small worklet chunks.
    for (int i = 0; i < 20000; i++) { int16_t s = i; mic.push(&s, 1); }
    assert(mic.read_frame(frame) == RebitMicBuffer::FrameSamples);
    for (int i = 0; i < RebitMicBuffer::FrameSamples; i++) assert(frame[i] == 20000 - RebitMicBuffer::Capacity + i);
    mic.clear(); // The blow override discards pending live input.
    assert(mic.read_frame(frame) == 0);
    puts("microphone buffer: inactive, PCM fidelity, underrun, overflow, wraparound, reset passed");
}
