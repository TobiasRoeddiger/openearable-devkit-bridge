#include "microphone_level.h"
#include <cassert>
#include <algorithm>
#include <cstdio>
int main() {
    for (unsigned rate : {8000u, 16000u}) {
        MicrophoneLevel level;
        int16_t x[160]; size_t n = rate / 100;
        double before=0, after=0; int peak=0;
        for (int block=0; block<200; block++) {
            for (size_t i=0;i<n;i++) x[i]=std::lround(1000*std::sin(2*3.14159265359*1000*(block*n+i)/rate));
            if(block>100)for(size_t i=0;i<n;i++)before+=(double)x[i]*x[i];
            level.process(x,n,rate,MicrophoneLevel::gainForVolume(8));
            if(block>100)for(size_t i=0;i<n;i++)after+=(double)x[i]*x[i];
        }
        double measured=20*std::log10(std::sqrt(after/before));
        assert(measured>11.5 && measured<12.1);
        for(int block=0;block<20;block++) {
            for(size_t i=0;i<n;i++)x[i]=(i%7<3)?32767:-32768;
            assert(level.process(x,n,rate,MicrophoneLevel::gainForVolume(15)));
            for(size_t i=0;i<n;i++)peak=std::max(peak,std::abs((int)x[i]));
        }
        assert(peak<=30000);
        level.process(x,n,rate,0);
        for(size_t i=0;i<n;i++)assert(x[i]==0);
        level.reset();
        std::fill(x,x+n,0);level.process(x,n,rate,16);
        for(size_t i=0;i<n;i++)assert(x[i]==0);
        printf("rate=%u nominal_gain_dB=%.3f max_limited_peak=%d PASS\n",rate,measured,peak);
    }
}
