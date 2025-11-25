void ExitToShell(void);

void __start(void) {
  char *argv[2] = {"app"};
  ExitToShell();
}
