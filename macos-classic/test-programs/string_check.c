void ExitToShell(void);

int check(char **strs) {
	// Check the first characters of the strings
	if (strs[0][0] == 'H' && strs[1][0] == 'W') {
		return 1;
	}
	return 0;
}

void __start(void) {
  char *strs[2] = {"Hello", "World"};
  if (check(strs) != 1) {
	// Error handling (infinite loop for simplicity)
	while (1)
	  ;
  }
  // Otherwise, exit the program properly
  ExitToShell();
}
