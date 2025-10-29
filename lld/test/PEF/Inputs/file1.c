// Section merging test - file 1
extern int func2(void);
extern int func3(void);

int data1 = 111;
const int rodata1 = 0x11111111;

int func1(void) {
    return 1;
}

int main(void) {
    return func1() + func2() + func3();
}
