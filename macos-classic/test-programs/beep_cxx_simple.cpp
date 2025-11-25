// Simple C++ test without global objects
extern "C" {
    void SysBeep(short duration);
    void Delay(long numTicks, long *finalTicks);
}

int main(int argc, char **argv) {
    SysBeep(30);
    Delay(60, 0);
    return 0;
}
