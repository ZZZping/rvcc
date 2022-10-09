int assert(int expected, int actual, char *code);
int printf(char *fmt, ...);
int sprintf(char *buf, char *fmt, ...);
int strcmp(char *p, char *q);
int memcmp(char *p, char *q, long n);

#

/* */ #

int main() {
  // [159] 支持空指示
  printf("OK\n");
  return 0;
}
