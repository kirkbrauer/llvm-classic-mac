// Multi-file test - main file
extern int add(int a, int b);
extern int multiply(int a, int b);

int global_data = 42;

int main(void)
{
    int result1 = add(5, 10);
    int result2 = multiply(3, 7);
    return result1 + result2;
}
