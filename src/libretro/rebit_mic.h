#ifndef REBIT_MIC_H
#define REBIT_MIC_H

#include <stdint.h>
#include <string.h>

// Main-thread web input only; never serialized or used by the wireless runtime.
// Keep at most 250 ms of 44.1 kHz mono PCM, preferring recent audio on overflow.
class RebitMicBuffer
{
public:
    static const int Capacity = 11025;
    static const int FrameSamples = 735;

    void set_active(bool on)
    {
        if (active != on) clear();
        active = on;
    }

    bool is_active() const { return active; }
    int size() const { return length; }
    void clear() { start = length = 0; }

    void push(const int16_t* samples, int count)
    {
        if (!active || !samples || count <= 0) return;
        if (count > Capacity)
        {
            samples += count - Capacity;
            count = Capacity;
        }
        for (int i = 0; i < count; i++)
        {
            if (length == Capacity)
            {
                start = (start + 1) % Capacity;
                length--;
            }
            buffer[(start + length) % Capacity] = samples[i];
            length++;
        }
    }

    int read_frame(int16_t* output)
    {
        memset(output, 0, FrameSamples * sizeof(int16_t));
        int count = length < FrameSamples ? length : FrameSamples;
        for (int i = 0; i < count; i++)
            output[i] = buffer[(start + i) % Capacity];
        start = (start + count) % Capacity;
        length -= count;
        return count;
    }

private:
    int16_t buffer[Capacity] = {};
    int start = 0;
    int length = 0;
    bool active = false;
};

#endif
