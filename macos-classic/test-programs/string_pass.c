void ExitToShell(void);

int hello(int* vals, char **strs) {
	return vals[2];
}

void __start(void) {
  int vals[3] = {1, 2, 3};
  char *strs[2] = {"Hello", "World"};
  hello(vals, strs);
  ExitToShell();
}
