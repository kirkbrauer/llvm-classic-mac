int global_data = 42;

int global_func() {
    return global_data;
}

int main() {
    return global_func();
}
